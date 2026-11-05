#include "alchemy/darwin/launchd/launchctl.hpp"

#include "alchemy/darwin/launchd/client.hpp"
#include "alchemy/darwin/launchd/protocol.hpp"

#include "alchemy/alchemy_root.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>

// The subcommands of launchctl(1) for the user's own domains (gui/<uid>,
// user/<uid>) and the legacy load/unload/list/start/stop/submit forms. The
// system domain is refused as for a user who is not root; subcommands about
// the macOS boot, the kernel or debugging are refused by name.
namespace alchemy::launchd {
namespace {

constexpr std::uint32_t uid = 1000;

std::string status_text(std::int32_t status) {
    static const std::map<std::int32_t, std::string> names{
        {1, "Operation not permitted"}, {2, "No such file or directory"}, {3, "No such process"}, {5, "Input/output error"},
        {8, "Exec format error"}, {13, "Permission denied"}, {22, "Invalid argument"}, {37, "Operation already in progress"},
        {45, "Operation not supported"}, {113, "Could not find specified service"}, {119, "Service is disabled"}};
    const auto found = names.find(status);
    return found == names.end() ? "Error " + std::to_string(status) : found->second;
}

std::optional<std::uint32_t> signal_number(std::string text) {
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return static_cast<std::uint32_t>(std::stoul(text));
    if (text.starts_with("SIG")) text = text.substr(3);
    static const std::map<std::string, std::uint32_t> names{
        {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"ILL", 4}, {"TRAP", 5}, {"ABRT", 6}, {"EMT", 7}, {"FPE", 8}, {"KILL", 9},
        {"BUS", 10}, {"SEGV", 11}, {"SYS", 12}, {"PIPE", 13}, {"ALRM", 14}, {"TERM", 15}, {"URG", 16}, {"STOP", 17},
        {"TSTP", 18}, {"CONT", 19}, {"CHLD", 20}, {"TTIN", 21}, {"TTOU", 22}, {"IO", 23}, {"XCPU", 24}, {"XFSZ", 25},
        {"VTALRM", 26}, {"PROF", 27}, {"WINCH", 28}, {"INFO", 29}, {"USR1", 30}, {"USR2", 31}};
    const auto found = names.find(text);
    if (found == names.end()) return std::nullopt;
    return found->second;
}

class Launchctl {
public:
    Launchctl(const ChildLaunch& launch, const std::vector<std::string>& options, const std::function<void(int, std::string_view)>& output)
        : launch_(launch), options_(options), output_(output) {
        cwd_ = launch.cwd.empty() ? std::string("/") : guest_path_string(launch.cwd);
        root_ = std::find(launch.envp.begin(), launch.envp.end(), std::string("ALCHEMY_SUPERUSER=1")) != launch.envp.end();
    }

    int run() {
        const auto& argv = launch_.argv;
        if (argv.size() < 2) { usage(); return 0; }
        const auto& command = argv[1];
        const std::vector<std::string> args(argv.begin() + 2, argv.end());
        if (command == "help" || command == "-h" || command == "--help") { usage(); return 0; }
        if (command == "version") { out("Alchemy session launchd (launchctl for the user's own domain)\n"); return 0; }
        if (command == "managerpid") { out("1\n"); return 0; }
        if (command == "manageruid") { out(std::to_string(uid) + "\n"); return 0; }
        if (command == "managername") { out("Aqua\n"); return 0; }
        if (command == "error") return error_command(args);
        // Root's legacy subcommands act on the system domain, which Alchemy does not run.
        static const std::vector<std::string> legacy{"load", "unload", "submit", "start", "stop", "remove"};
        if (root_ && std::find(legacy.begin(), legacy.end(), command) != legacy.end()) {
            err("launchctl " + command + ": as root this acts on the system domain; Alchemy runs no system launchd\n");
            return 45;
        }
        if (root_ && command == "list") { out("PID\tStatus\tLabel\n"); return 0; }
        if (command == "bootstrap") return bootstrap(args);
        if (command == "bootout") return bootout(args);
        if (command == "load") return load(args);
        if (command == "unload") return unload(args);
        if (command == "enable" || command == "disable") return enable(args, command == "enable");
        if (command == "kickstart") return kickstart(args);
        if (command == "kill") return kill(args);
        if (command == "print") return print(args);
        if (command == "print-disabled") return print_disabled(args);
        if (command == "list") return list(args);
        if (command == "start" || command == "stop") return start_stop(args, command == "start");
        if (command == "remove") return remove(args);
        if (command == "submit") return submit(args);
        if (command == "setenv") return setenv(args);
        if (command == "unsetenv") return unsetenv(args);
        if (command == "getenv") return getenv(args);
        if (command == "asuser") { err("Usage: launchctl asuser <UID> <program> [arguments]\n"); return 64; }
        static const std::vector<std::string> refused{"reboot", "bsexec", "bslist", "config", "dumpstate", "dumpjpcategory", "procinfo",
            "hostinfo", "resolveport", "limit", "examine", "attach", "debug", "blame", "plist", "userswitch", "kmutil", "runstats",
            "print-cache", "dump-xsc", "uncache", "reload", "load-adaptive", "bootshell", "umask", "log", "variant"};
        if (std::find(refused.begin(), refused.end(), command) != refused.end()) {
            err("launchctl " + command + ": not supported under Alchemy (it has a session launchd for the user's own jobs only)\n");
            return 45;
        }
        err("Unrecognized subcommand: " + command + "\n");
        usage();
        return 64;
    }

private:
    void out(std::string_view text) { output_(1, text); }
    void err(std::string_view text) { output_(2, text); }

    void usage() {
        out("Usage: launchctl <subcommand> ... | help [subcommand]\n"
            "Subcommands under Alchemy (the user's own gui/<uid> and user/<uid> domains):\n"
            "\tbootstrap, bootout, enable, disable, kickstart, kill, print, print-disabled,\n"
            "\tload, unload, list, start, stop, remove, submit, setenv, unsetenv, getenv,\n"
            "\tmanagerpid, manageruid, managername, asuser, error, version, help\n");
    }

    Session* session(bool start) {
        if (session_) return session_.get();
        if (!start && !Session::running()) return nullptr;
        std::string why;
        session_ = Session::open(start, options_, &why);
        if (!session_ && start) err("launchctl: cannot reach the session launchd: " + why + "\n");
        return session_.get();
    }

    // "gui/1000", "user/1000", "system", "pid/N", "login/N": the user's own
    // domains are the session; the system domain needs root.
    enum class Domain { session, system, other_user, bad };
    Domain domain_of(const std::string& text, std::string* label = nullptr) {
        const auto slash = text.find('/');
        const auto kind = text.substr(0, slash);
        std::string rest = slash == std::string::npos ? std::string() : text.substr(slash + 1);
        if (kind == "system") {
            if (label) *label = rest;
            return Domain::system;
        }
        if (kind == "gui" || kind == "user" || kind == "login" || kind == "pid") {
            const auto next = rest.find('/');
            const auto id = rest.substr(0, next);
            if (label) *label = next == std::string::npos ? std::string() : rest.substr(next + 1);
            if (id.empty() || !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; })) return Domain::bad;
            if ((kind == "gui" || kind == "user") && std::stoul(id) != uid) return Domain::other_user;
            return Domain::session;
        }
        return Domain::bad;
    }

    // A failure of a domain-target for a user who is not root.
    int refuse_domain(const char* what, Domain domain, const std::string& text) {
        if (domain == Domain::bad) { err(std::string(what) + ": bad domain or service target: " + text + "\n"); return 22; }
        err(std::string(what) + " failed: 1: Operation not permitted" +
            (domain == Domain::system ? " (the system domain needs root; Alchemy runs no system launchd)\n" : "\n"));
        return 1;
    }

    std::string absolute(const std::string& path) const {
        if (path.starts_with('/')) return path;
        return cwd_ + (cwd_.ends_with('/') ? "" : "/") + path;
    }

    std::filesystem::path host_of(const std::string& guest) const {
        const auto normal = utf8_path(guest).lexically_normal().generic_u8string();
        const std::string generic(reinterpret_cast<const char*>(normal.data()), normal.size());
        if (generic.starts_with("/Volumes/") && generic.size() >= 10) {
            std::string native{generic[9], ':', '/'};
            if (generic.size() > 11) native += generic.substr(11);
            return utf8_path(native);
        }
        if (generic == "/tmp" || generic.starts_with("/tmp/")) {
            auto temporary = std::filesystem::temp_directory_path();
            if (generic.size() > 5) temporary /= utf8_path(generic.substr(5));
            return temporary;
        }
        if (auto in_root = guest_root_host_path(generic)) return *in_root;
        return std::filesystem::current_path().root_path() / utf8_path(generic).relative_path();
    }

    // The plist files a path names (a directory: its *.plist files).
    std::vector<std::string> plists(const std::string& path) {
        const auto guest = absolute(path);
        const auto host = host_of(guest);
        std::error_code error;
        if (!std::filesystem::is_directory(host, error)) return {guest};
        std::vector<std::string> out;
        for (const auto& entry : std::filesystem::directory_iterator(host, error))
            if (entry.path().extension() == ".plist") out.push_back(guest + "/" + path_utf8(entry.path().filename()));
        std::sort(out.begin(), out.end());
        return out;
    }

    std::optional<Value> read(const std::string& guest, const char* what, int& status) {
        try {
            return read_plist_file(path_utf8(host_of(guest)));
        } catch (const std::exception& error) {
            std::error_code ignored;
            status = std::filesystem::exists(host_of(guest), ignored) ? 22 : 2;
            err(guest + ": " + (status == 2 ? std::string("No such file or directory") : std::string("Invalid property list: ") + error.what()) + "\n");
            err(std::string(what) + " failed: " + std::to_string(status) + ": " + status_text(status) + "\n");
            return std::nullopt;
        }
    }

    int load_paths(const std::vector<std::string>& paths, std::uint8_t flags, const char* what) {
        if (paths.empty()) { err(std::string("Usage: launchctl ") + (flags & wire::load_legacy ? "load [-wF] <path> ..." : "bootstrap <domain-target> <path> ...") + "\n"); return 64; }
        auto* launchd = session(true);
        if (!launchd) return 5;
        int result = 0;
        for (const auto& path : paths) {
            for (const auto& file : plists(path)) {
                int status = 0;
                const auto value = read(file, what, status);
                if (!value) { result = status; continue; }
                std::string message;
                status = launchd->load(*value, file, flags, message);
                if (status) {
                    if (!message.empty()) err(file + ": " + message + "\n");
                    err(std::string(what) + " failed: " + std::to_string(status) + ": " + status_text(status) + "\n");
                    result = status;
                }
            }
        }
        return result;
    }

    std::optional<std::string> label_of_file(const std::string& guest) {
        int status = 0;
        const auto value = read(guest, "Unload", status);
        if (!value) return std::nullopt;
        const auto* label = value->find("Label");
        if (!label || !label->is(Value::Kind::string)) { err(guest + ": no Label\n"); return std::nullopt; }
        return label->text;
    }

    int bootstrap(const std::vector<std::string>& args) {
        if (args.empty()) { err("Usage: launchctl bootstrap <domain-target> <path> ...\n"); return 64; }
        const auto domain = domain_of(args[0]);
        if (domain != Domain::session) return refuse_domain("Bootstrap", domain, args[0]);
        return load_paths({args.begin() + 1, args.end()}, 0, "Bootstrap");
    }

    int remove_label(const std::string& label, const char* what) {
        auto* launchd = session(false);
        std::string message;
        const auto status = launchd ? launchd->remove(label, message) : wire::status_no_such_service;
        if (status) err(std::string(what) + " failed: " + std::to_string(status) + ": " + status_text(status) + "\n");
        return status;
    }

    int bootout(const std::vector<std::string>& args) {
        if (args.empty()) { err("Usage: launchctl bootout <domain-target> [<path> ...] | <service-target>\n"); return 64; }
        std::string label;
        const auto domain = domain_of(args[0], &label);
        if (domain != Domain::session) return refuse_domain("Boot-out", domain, args[0]);
        if (!label.empty()) return remove_label(label, "Boot-out");
        if (args.size() == 1) { err("Boot-out failed: 45: tearing down a whole domain is not supported under Alchemy\n"); return 45; }
        int result = 0;
        for (std::size_t i = 1; i < args.size(); ++i)
            for (const auto& file : plists(args[i])) {
                const auto found = label_of_file(file);
                if (!found) { result = 2; continue; }
                if (const auto status = remove_label(*found, "Boot-out")) result = status;
            }
        return result;
    }

    int load(const std::vector<std::string>& args) {
        std::uint8_t flags = wire::load_legacy;
        std::vector<std::string> paths;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg == "-w") flags |= wire::load_write;
            else if (arg == "-F") flags |= wire::load_force;
            else if (arg == "-wF" || arg == "-Fw") flags |= wire::load_write | wire::load_force;
            else if (arg == "-S" || arg == "-D") {
                if (++i >= args.size()) { err("launchctl load: " + arg + " needs a value\n"); return 64; }
                if (arg == "-D" && (args[i] == "system" || args[i] == "network")) return refuse_domain("Load", Domain::system, args[i]);
            } else paths.push_back(arg);
        }
        return load_paths(paths, flags, "Load");
    }

    int unload(const std::vector<std::string>& args) {
        bool write = false;
        std::vector<std::string> paths;
        for (const auto& arg : args) {
            if (arg == "-w") write = true;
            else if (arg == "-F" || arg == "-wF") write = write || arg == "-wF";
            else paths.push_back(arg);
        }
        if (paths.empty()) { err("Usage: launchctl unload [-w] <path> ...\n"); return 64; }
        int result = 0;
        for (const auto& path : paths)
            for (const auto& file : plists(path)) {
                const auto label = label_of_file(file);
                if (!label) { result = 2; continue; }
                if (write) {
                    if (auto* launchd = session(true)) { std::string message; static_cast<void>(launchd->set_enabled(*label, false, message)); }
                }
                if (const auto status = remove_label(*label, "Unload")) result = status;
            }
        return result;
    }

    int enable(const std::vector<std::string>& args, bool enabled) {
        if (args.size() != 1) { err(std::string("Usage: launchctl ") + (enabled ? "enable" : "disable") + " <service-target>\n"); return 64; }
        std::string label;
        const auto domain = domain_of(args[0], &label);
        if (domain != Domain::session) return refuse_domain(enabled ? "Enable" : "Disable", domain, args[0]);
        if (label.empty()) { err("a service-target (domain/label) is required\n"); return 22; }
        auto* launchd = session(true);
        if (!launchd) return 5;
        std::string message;
        return launchd->set_enabled(label, enabled, message);
    }

    int kickstart(const std::vector<std::string>& args) {
        bool kill_first = false, print_pid = false;
        std::string target;
        for (const auto& arg : args) {
            if (arg.starts_with('-')) {
                for (const auto c : arg.substr(1)) {
                    if (c == 'k') kill_first = true;
                    else if (c == 'p') print_pid = true;
                    else if (c == 's') {}   // start suspended: not supported, the job starts normally
                    else { err(std::string("launchctl kickstart: unknown option -") + c + "\n"); return 64; }
                }
            } else target = arg;
        }
        std::string label;
        const auto domain = domain_of(target, &label);
        if (domain != Domain::session) return refuse_domain("Kickstart", domain, target);
        if (label.empty()) { err("a service-target (domain/label) is required\n"); return 22; }
        auto* launchd = session(true);
        if (!launchd) return 5;
        std::string message;
        std::uint32_t pid = 0;
        const auto status = launchd->kickstart(label, kill_first, pid, message);
        if (status) {
            if (!message.empty()) err(message + "\n");
            err("Could not kickstart service \"" + label + "\": " + std::to_string(status) + ": " + status_text(status) + "\n");
            return status;
        }
        if (print_pid) out(std::to_string(pid) + "\n");
        return 0;
    }

    int kill(const std::vector<std::string>& args) {
        if (args.size() != 2) { err("Usage: launchctl kill <signal-name|signal-number> <service-target>\n"); return 64; }
        const auto signal = signal_number(args[0]);
        if (!signal) { err("Unrecognized signal: " + args[0] + "\n"); return 22; }
        std::string label;
        const auto domain = domain_of(args[1], &label);
        if (domain != Domain::session) return refuse_domain("Kill", domain, args[1]);
        auto* launchd = session(false);
        std::string message;
        const auto status = launchd ? launchd->signal(label, *signal, message) : wire::status_no_such_service;
        if (status) err("Could not kill service \"" + label + "\": " + std::to_string(status) + ": " + (message.empty() ? status_text(status) : message) + "\n");
        return status;
    }

    static std::string field(const std::string& key, const Value& value, const std::string& pad) {
        if (value.is(Value::Kind::array)) {
            std::string text = pad + key + " = {\n";
            for (const auto& item : value.array) text += pad + "\t" + (item.is(Value::Kind::string) ? item.text : to_text(item)) + "\n";
            return text + pad + "}\n";
        }
        if (value.is(Value::Kind::dictionary)) {
            std::string text = pad + key + " = {\n";
            for (const auto& [name, member] : value.dictionary) text += pad + "\t" + name + " => " + (member.is(Value::Kind::string) ? member.text : to_text(member)) + "\n";
            return text + pad + "}\n";
        }
        return pad + key + " = " + (value.is(Value::Kind::string) ? value.text : to_text(value)) + "\n";
    }

    int print(const std::vector<std::string>& args) {
        if (args.size() != 1) { err("Usage: launchctl print <domain-target> | <service-target>\n"); return 64; }
        std::string label;
        const auto domain = domain_of(args[0], &label);
        if (domain != Domain::session) return refuse_domain("Print", domain, args[0]);
        auto* launchd = session(false);
        const auto slash = args[0].find('/');
        const auto domain_name = label.empty() ? args[0] : args[0].substr(0, args[0].find('/', slash + 1));
        if (!label.empty()) {
            const auto info = launchd ? launchd->info(label) : std::nullopt;
            if (!info) {
                err("Could not find service \"" + label + "\" in domain for user gui: " + std::to_string(uid) + "\n");
                return wire::status_no_such_service;
            }
            const auto* pid = info->state.find("pid");
            std::string text = args[0] + " = {\n\tactive count = " + (pid ? "1" : "0") + "\n";
            for (const auto& [key, value] : info->state.dictionary) text += field(key, value, "\t");
            out(text + "}\n");
            return 0;
        }
        std::string text = domain_name + " = {\n\ttype = login\n\thandle = " + std::to_string(uid) + "\n\tcreator = Alchemy session launchd\n";
        const auto entries = launchd ? launchd->list() : std::vector<ListEntry>{};
        text += "\tactive count = " + std::to_string(std::count_if(entries.begin(), entries.end(), [](const ListEntry& e) { return e.pid > 0; })) + "\n";
        text += "\tservices = {\n";
        for (const auto& entry : entries)
            text += "\t\t" + std::to_string(entry.pid > 0 ? entry.pid : 0) + "\t" + std::to_string(entry.last_status) + "\t" + entry.label + "\n";
        text += "\t}\n";
        if (launchd) {
            text += "\tdisabled services = {\n";
            for (const auto& [name, disabled] : launchd->overrides()) text += "\t\t\"" + name + "\" => " + (disabled ? "disabled" : "enabled") + "\n";
            text += "\t}\n";
        }
        out(text + "}\n");
        return 0;
    }

    int print_disabled(const std::vector<std::string>& args) {
        if (args.size() != 1) { err("Usage: launchctl print-disabled <domain-target>\n"); return 64; }
        const auto domain = domain_of(args[0]);
        if (domain != Domain::session) return refuse_domain("Print", domain, args[0]);
        auto* launchd = session(false);
        std::string text = "disabled services = {\n";
        if (launchd) for (const auto& [name, disabled] : launchd->overrides()) text += "\t\"" + name + "\" => " + (disabled ? "disabled" : "enabled") + "\n";
        out(text + "}\n");
        return 0;
    }

    int list(const std::vector<std::string>& args) {
        auto* launchd = session(false);
        if (args.empty()) {
            std::string text = "PID\tStatus\tLabel\n";
            if (launchd)
                for (const auto& entry : launchd->list())
                    text += (entry.pid > 0 ? std::to_string(entry.pid) : std::string("-")) + "\t" + std::to_string(entry.last_status) + "\t" + entry.label + "\n";
            out(text);
            return 0;
        }
        const auto info = launchd ? launchd->info(args[0]) : std::nullopt;
        if (!info) {
            err("Could not find service \"" + args[0] + "\" in domain for port\n");
            return wire::status_no_such_service;
        }
        out(to_text(info->dictionary) + ";\n");
        return 0;
    }

    int start_stop(const std::vector<std::string>& args, bool start) {
        if (args.size() != 1) { err(std::string("Usage: launchctl ") + (start ? "start" : "stop") + " <label>\n"); return 64; }
        auto* launchd = session(start);
        std::string message;
        const auto status = !launchd ? wire::status_no_such_service : start ? launchd->start(args[0], message) : launchd->stop(args[0], message);
        if (status) err(std::string(start ? "Start" : "Stop") + " failed: " + std::to_string(status) + ": " + status_text(status) + "\n");
        return status;
    }

    int remove(const std::vector<std::string>& args) {
        if (args.size() != 1) { err("Usage: launchctl remove <label>\n"); return 64; }
        return remove_label(args[0], "Remove");
    }

    int submit(const std::vector<std::string>& args) {
        auto job = Value::of_dictionary();
        std::vector<Value> command;
        std::size_t i = 0;
        for (; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg == "--") { ++i; break; }
            const auto value = [&]() -> std::optional<std::string> { if (i + 1 < args.size()) return args[++i]; return std::nullopt; };
            std::optional<std::string> text;
            if (arg == "-l" && (text = value())) job.set("Label", Value::of_string(*text));
            else if (arg == "-p" && (text = value())) job.set("Program", Value::of_string(absolute(*text)));
            else if (arg == "-o" && (text = value())) job.set("StandardOutPath", Value::of_string(absolute(*text)));
            else if (arg == "-e" && (text = value())) job.set("StandardErrorPath", Value::of_string(absolute(*text)));
            else { err("Usage: launchctl submit -l <label> [-p <program>] [-o <path>] [-e <path>] -- <command> [args]\n"); return 64; }
        }
        for (; i < args.size(); ++i) command.push_back(Value::of_string(args[i]));
        if (!job.find("Label") || command.empty()) { err("Usage: launchctl submit -l <label> [-p <program>] [-o <path>] [-e <path>] -- <command> [args]\n"); return 64; }
        job.set("ProgramArguments", Value::of_array(std::move(command)));
        // A submitted program is kept alive.
        job.set("KeepAlive", Value::of_boolean(true));
        job.set("RunAtLoad", Value::of_boolean(true));
        auto* launchd = session(true);
        if (!launchd) return 5;
        std::string message;
        const auto status = launchd->load(job, "", wire::load_submit, message);
        if (status) err("Submit failed: " + std::to_string(status) + ": " + (message.empty() ? status_text(status) : message) + "\n");
        return status;
    }

    int setenv(const std::vector<std::string>& args) {
        if (args.empty() || args.size() % 2) { err("Usage: launchctl setenv <key> <value> [<key> <value> ...]\n"); return 64; }
        auto* launchd = session(true);
        if (!launchd) return 5;
        for (std::size_t i = 0; i < args.size(); i += 2) launchd->set_variable(args[i], args[i + 1]);
        return 0;
    }

    int unsetenv(const std::vector<std::string>& args) {
        if (args.empty()) { err("Usage: launchctl unsetenv <key> ...\n"); return 64; }
        if (auto* launchd = session(false)) for (const auto& key : args) launchd->set_variable(key, std::nullopt);
        return 0;
    }

    int getenv(const std::vector<std::string>& args) {
        if (args.size() != 1) { err("Usage: launchctl getenv <key>\n"); return 64; }
        auto* launchd = session(false);
        if (const auto value = launchd ? launchd->variable(args[0]) : std::nullopt) out(*value + "\n");
        return 0;
    }

    int error_command(const std::vector<std::string>& args) {
        if (args.empty()) { err("Usage: launchctl error <code>\n"); return 64; }
        const auto& text = args.back();
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) { err("bad error code\n"); return 64; }
        out(status_text(static_cast<std::int32_t>(std::stol(text))) + "\n");
        return 0;
    }

    const ChildLaunch& launch_;
    const std::vector<std::string>& options_;
    const std::function<void(int, std::string_view)>& output_;
    std::string cwd_;
    bool root_{};   // ALCHEMY_SUPERUSER=1: root's legacy subcommands mean the system domain
    std::unique_ptr<Session> session_;
};

} // namespace

int launchctl_main(const ChildLaunch& launch, const std::vector<std::string>& launch_options,
                   const std::function<void(int, std::string_view)>& output) {
    // The tool child has no guest program: its root comes from the
    // environment or from the program that ran launchctl.
    if (!configured_alchemy_root()) {
        std::optional<std::filesystem::path> root;
        if (const auto value = host::process::environment("ALCHEMY_ROOT"); value && !value->empty()) root = utf8_path(*value);
        else if (!launch.host_program.empty()) root = find_alchemy_root(launch.host_program);
        if (root) {
            try { configure_alchemy_root(*root); } catch (const std::exception&) {}
        }
    }
    try {
        return Launchctl(launch, launch_options, output).run();
    } catch (const std::exception& error) {
        output(2, std::string("launchctl: ") + error.what() + "\n");
        return 5;
    }
}

} // namespace alchemy::launchd
