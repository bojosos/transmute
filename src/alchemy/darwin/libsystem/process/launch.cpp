// Darwin child processes over host::process: launch specification, target
// resolution, process creation in a kill-on-close job, and the built-in
// shell (include/alchemy/darwin_process.hpp, docs/ALCHEMY_PROCESS.md).
#define _CRT_SECURE_NO_WARNINGS
#include "alchemy/darwin_process.hpp"
#include "alchemy/darwin/libsystem/errno.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/vm.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <thread>

namespace alchemy {
namespace {

constexpr std::uint32_t eperm = 1, enoent = 2, eio = 5, e2big = 7, enoexec = 8, ebadf = 9, enomem = 12,
    eacces = 13, einval = 22, enotdir = 20, eisdir = 21, emfile = 24, enametoolong = 63, enosys = 78;

std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

std::string text_of(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::optional<std::string> env_value(const std::vector<std::string>& envp, std::string_view key) {
    for (const auto& entry : envp)
        if (entry.size() > key.size() && entry.compare(0, key.size(), key) == 0 && entry[key.size()] == '=')
            return entry.substr(key.size() + 1);
    return std::nullopt;
}

void env_set(std::vector<std::string>& envp, std::string_view key, std::string_view value) {
    const auto entry = std::string(key) + "=" + std::string(value);
    for (auto& item : envp)
        if (item.size() > key.size() && item.compare(0, key.size(), key) == 0 && item[key.size()] == '=') { item = entry; return; }
    envp.push_back(entry);
}

void env_unset(std::vector<std::string>& envp, std::string_view key) {
    std::erase_if(envp, [&](const std::string& item) {
        return item.size() > key.size() && item.compare(0, key.size(), key) == 0 && item[key.size()] == '=';
    });
}

// A path in the host's own form (native separators), UTF-8.
std::string native_text(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ---- launch specification -------------------------------------------------------
constexpr std::array<char, 8> spec_magic{'A', 'L', 'C', 'H', 'S', 'P', 'C', '2'};

class SpecWriter {
public:
    void u32(std::uint32_t value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
    void u64(std::uint64_t value) { u32(static_cast<std::uint32_t>(value)); u32(static_cast<std::uint32_t>(value >> 32)); }
    void blob(std::span<const std::uint8_t> value) { u32(static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end()); }
    void string(std::string_view value) { blob(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())); }
    void strings(const std::vector<std::string>& values) { u32(static_cast<std::uint32_t>(values.size())); for (const auto& value : values) string(value); }
    std::vector<std::uint8_t> bytes;
};

class SpecReader {
public:
    explicit SpecReader(std::span<const std::uint8_t> value) : data(value) {}
    std::uint32_t u32() {
        need(4); std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4; return value;
    }
    std::uint64_t u64() { const auto low = u32(); return low | (static_cast<std::uint64_t>(u32()) << 32); }
    std::vector<std::uint8_t> blob() { const auto size = u32(); need(size); std::vector<std::uint8_t> value(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + size)); at += size; return value; }
    std::string string() { const auto value = blob(); return {value.begin(), value.end()}; }
    std::vector<std::string> strings() { const auto count = u32(); if (count > (1U << 20)) bad(); std::vector<std::string> values; for (std::uint32_t i = 0; i < count; ++i) values.push_back(string()); return values; }
private:
    void need(std::size_t count) { if (count > data.size() - at) bad(); }
    [[noreturn]] static void bad() { throw std::runtime_error("Alchemy child launch specification is malformed"); }
    std::span<const std::uint8_t> data;
    std::size_t at{};
};

std::vector<std::uint8_t> encode(const ChildLaunch& launch) {
    SpecWriter w;
    for (const auto c : spec_magic) w.bytes.push_back(static_cast<std::uint8_t>(c));
    w.string(launch.tool);
    w.string(text_of(launch.host_program));
    w.string(launch.guest_program);
    w.strings(launch.argv);
    w.strings(launch.envp);
    w.string(text_of(launch.cwd));
    w.u32(launch.pid); w.u32(launch.ppid); w.u32(launch.pgid); w.u32(launch.sid);
    w.u32(launch.umask); w.u32(launch.ignored_signals); w.u32(launch.signal_mask);
    w.u32(static_cast<std::uint32_t>(launch.fds.size()));
    for (const auto& fd : launch.fds) {
        w.u32(static_cast<std::uint32_t>(fd.fd)); w.u32(static_cast<std::uint32_t>(fd.kind));
        w.u64(fd.handle); w.u64(fd.flags); w.u32(static_cast<std::uint32_t>(fd.standard));
        w.string(fd.path); w.blob(fd.blob);
        w.u32(static_cast<std::uint32_t>(fd.socket_domain)); w.u32(static_cast<std::uint32_t>(fd.socket_type));
        w.u32(static_cast<std::uint32_t>(fd.socket_protocol));
        w.u32(fd.terminal); w.u32(fd.terminal_master ? 1U : 0U);
    }
    return std::move(w.bytes);
}

ChildLaunch decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < spec_magic.size() || std::memcmp(bytes.data(), spec_magic.data(), spec_magic.size()) != 0)
        throw std::runtime_error("Alchemy child launch specification has an unknown format");
    SpecReader r(bytes.subspan(spec_magic.size()));
    ChildLaunch launch;
    launch.tool = r.string();
    launch.host_program = utf8_path(r.string());
    launch.guest_program = r.string();
    launch.argv = r.strings();
    launch.envp = r.strings();
    launch.cwd = utf8_path(r.string());
    launch.pid = r.u32(); launch.ppid = r.u32(); launch.pgid = r.u32(); launch.sid = r.u32();
    launch.umask = r.u32(); launch.ignored_signals = r.u32(); launch.signal_mask = r.u32();
    const auto count = r.u32();
    if (count > 4096) throw std::runtime_error("Alchemy child launch specification has too many descriptors");
    for (std::uint32_t i = 0; i < count; ++i) {
        ChildFd fd;
        fd.fd = static_cast<int>(r.u32()); fd.kind = static_cast<ChildFdKind>(r.u32());
        fd.handle = r.u64(); fd.flags = r.u64(); fd.standard = static_cast<int>(r.u32());
        fd.path = r.string(); fd.blob = r.blob();
        fd.socket_domain = static_cast<std::int32_t>(r.u32()); fd.socket_type = static_cast<std::int32_t>(r.u32());
        fd.socket_protocol = static_cast<std::int32_t>(r.u32());
        fd.terminal = r.u32(); fd.terminal_master = r.u32() != 0;
        launch.fds.push_back(std::move(fd));
    }
    return launch;
}

// ---- guest paths ------------------------------------------------------------------------------
bool is_shell_path(std::string_view path) {
    return path == "/bin/sh" || path == "/bin/bash" || path == "/usr/bin/sh" || path == "/usr/bin/bash" ||
        path == "/bin/zsh" || path == "/usr/bin/zsh" || path == "/bin/dash";
}
bool is_env_path(std::string_view path) { return path == "/usr/bin/env" || path == "/bin/env"; }
// launchctl: only "asuser <uid> <program> [arguments]" (resolve_exec).
bool is_launchctl_path(std::string_view path) { return path == "/bin/launchctl"; }
// macOS system programs Alchemy answers itself (run_child_tool): their guest
// binaries do not exist, but installers call them.
std::string_view system_tool(std::string_view path) {
    if (path == "/usr/libexec/ApplicationFirewall/socketfilterfw") return "socketfilterfw";
    return {};
}
bool is_python_path(std::string_view path) {
    for (const std::string_view directory : {"/usr/bin/", "/usr/local/bin/", "/opt/homebrew/bin/"}) {
        if (!path.starts_with(directory)) continue;
        const auto name = path.substr(directory.size());
        if (name == "python" || name == "python3") return true;
        if (name.starts_with("python3.") && name.size() > 8 &&
            std::all_of(name.begin() + 8, name.end(), [](char c) { return c >= '0' && c <= '9'; })) return true;
    }
    return false;
}

enum class FileKind { missing, directory, bundle, macho, script, windows, other };

FileKind classify(const std::filesystem::path& host, std::string* first_line = nullptr) {
    std::error_code error;
    const auto status = std::filesystem::status(host, error);
    if (error || !std::filesystem::exists(status)) return FileKind::missing;
    if (std::filesystem::is_directory(status)) {
        const auto name = lower(text_of(host.filename()));
        return name.ends_with(".app") ? FileKind::bundle : FileKind::directory;
    }
    std::ifstream input(host, std::ios::binary);
    std::array<char, 512> head{};
    input.read(head.data(), head.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count >= 4) {
        const auto b = reinterpret_cast<const unsigned char*>(head.data());
        const std::uint32_t le = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        if (le == 0xfeedfacfU || le == 0xfeedfaceU || le == 0xbebafecaU || le == 0xbfbafecaU) return FileKind::macho;
    }
    if (count >= 2 && head[0] == '#' && head[1] == '!') {
        if (first_line) {
            std::string line(head.data() + 2, count - 2);
            const auto end = line.find('\n');
            if (end == std::string::npos && count == head.size())
                throw ProcessError(enoexec, "#! line of " + text_of(host) + " exceeds 510 bytes");
            if (end != std::string::npos) line.resize(end);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            *first_line = line;
        }
        return FileKind::script;
    }
    if (count >= 2 && head[0] == 'M' && head[1] == 'Z') return FileKind::windows;
    return FileKind::other;
}

std::optional<std::filesystem::path> python_alias(const ExecEnvironment& environment) {
    if (const auto value = env_value(environment.host_environment, "ALCHEMY_GUEST_PYTHON"); value && !value->empty()) {
        const auto host = utf8_path(*value);
        if (classify(host) == FileKind::macho) return host;
    }
    const auto& main = environment.main_executable;
    if (main.empty()) return std::nullopt;
    if (lower(text_of(main.filename())).starts_with("python3")) return main;
    // The bundled interpreter of an application: <X.app>/Contents/Resources/<version>/python/bin/python3*.
    for (auto at = main.parent_path(); !at.empty() && at != at.parent_path(); at = at.parent_path()) {
        if (lower(text_of(at.filename())) != "contents" || !lower(text_of(at.parent_path().filename())).ends_with(".app")) continue;
        std::error_code error;
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator version(at / "Resources", error), end; !error && version != end; version.increment(error)) {
            const auto bin = version->path() / "python" / "bin";
            std::error_code inner;
            for (std::filesystem::directory_iterator file(bin, inner), last; !inner && file != last; file.increment(inner)) {
                const auto name = lower(text_of(file->path().filename()));
                if (name.starts_with("python3") && classify(file->path()) == FileKind::macho) found.push_back(file->path());
            }
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.back();   // python3.11 sorts after python3
        break;
    }
    return std::nullopt;
}

std::vector<std::string> split_path(const std::vector<std::string>& envp) {
    const auto value = env_value(envp, "PATH").value_or("/usr/bin:/bin");
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = value.find(':', start);
        result.push_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

} // namespace

std::uint32_t darwin_wait_status(std::uint32_t code) noexcept {
    if ((code & 0xffffff00U) == child_signal_exit_marker) return code & 0x7f;
    if (code >= 0xc0000000U) {
        switch (code) {
        case 0xc0000005U: case 0xc00000fdU: return 11;   // access violation, stack overflow: SIGSEGV
        case 0xc000001dU: case 0xc0000096U: return 4;    // illegal or privileged instruction: SIGILL
        case 0xc0000094U: case 0xc0000095U: return 8;    // integer divide by zero, overflow: SIGFPE
        case 0xc000013aU: return 2;                      // console Ctrl+C: SIGINT
        case 0xc0000409U: return 6;                      // fail-fast: SIGABRT
        default: return 9;                               // ended externally: SIGKILL
        }
    }
    return (code & 0xff) << 8;
}

bool virtual_program(std::string_view guest_path, const ExecEnvironment& environment) {
    if (is_shell_path(guest_path) || is_env_path(guest_path) || is_launchctl_path(guest_path) || !system_tool(guest_path).empty()) return true;
    return is_python_path(guest_path) && python_alias(environment).has_value();
}

bool guest_executable(std::string_view guest_path, const ExecEnvironment& environment) {
    if (is_shell_path(guest_path) || is_env_path(guest_path) || is_launchctl_path(guest_path) || !system_tool(guest_path).empty()) return true;
    if (is_python_path(guest_path)) return python_alias(environment).has_value();
    try {
        const auto kind = classify(environment.map_path(guest_path));
        return kind == FileKind::macho || kind == FileKind::script || kind == FileKind::bundle;
    } catch (...) { return false; }
}

std::string search_path(std::string_view file, const std::vector<std::string>& envp, const ExecEnvironment& environment) {
    if (file.empty()) throw ProcessError(enoent, "empty program name");
    if (file.find('/') != std::string_view::npos) return std::string(file);
    bool denied = false;
    for (const auto& directory : split_path(envp)) {
        const auto candidate = (directory.empty() ? std::string(".") : directory) + "/" + std::string(file);
        if (is_shell_path(candidate) || is_env_path(candidate) || is_launchctl_path(candidate) || !system_tool(candidate).empty()) return candidate;
        if (is_python_path(candidate)) { if (python_alias(environment)) return candidate; continue; }
        try {
            const auto host = environment.map_path(candidate);
            const auto kind = classify(host);
            if (kind == FileKind::missing) continue;
            if (kind == FileKind::directory) { denied = true; continue; }
            return candidate;
        } catch (const ProcessError&) { continue; } catch (const std::exception&) { continue; }
    }
    throw ProcessError(denied ? eacces : enoent, std::string(file) + ": not found in PATH");
}

ExecTarget resolve_exec(std::string_view guest_path, std::vector<std::string> argv, std::vector<std::string>& envp,
                        const ExecEnvironment& environment) {
    std::string path(guest_path);
    for (int depth = 0; depth < 6; ++depth) {
        if (path.empty()) throw ProcessError(enoent, "empty program path");
        if (is_shell_path(path)) return {"sh", {}, path, std::move(argv)};
        if (const auto tool = system_tool(path); !tool.empty()) return {std::string(tool), {}, path, std::move(argv)};
        if (is_env_path(path)) {
            // env [-i] [NAME=VALUE ...] [utility [argument ...]], resolved here
            // (the env process would exec the utility with the same pid).
            std::size_t index = 1;
            for (; index < argv.size(); ++index) {
                const auto& item = argv[index];
                if (item == "-i" || item == "-") { envp.clear(); continue; }
                if (item == "-u" && index + 1 < argv.size()) { env_unset(envp, argv[++index]); continue; }
                if (item == "--") { ++index; break; }
                if (item.starts_with('-')) throw ProcessError(einval, "env: unsupported option " + item);
                const auto equals = item.find('=');
                if (equals == std::string::npos || equals == 0) break;
                env_set(envp, item.substr(0, equals), item.substr(equals + 1));
            }
            if (index >= argv.size()) return {"env", {}, path, std::move(argv)};
            path = search_path(argv[index], envp, environment);
            argv.erase(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (is_launchctl_path(path)) {
            // launchctl asuser <uid> <program> [arguments]: the program in that
            // user's launchd bootstrap. Alchemy has one user session, so for the
            // caller's own user (or from root) it is the program itself, resolved
            // here as exec would; other launchctl uses need launchd.
            if (argv.size() < 4 || argv[1] != "asuser")
                throw ProcessError(enoexec, "launchctl " + (argv.size() > 1 ? argv[1] : std::string()) +
                    ": unsupported (no launchd; only 'asuser <uid> <program>' runs)");
            const auto& user = argv[2];
            if (user.empty() || user.size() > 10 || !std::all_of(user.begin(), user.end(), [](char c) { return c >= '0' && c <= '9'; }))
                throw ProcessError(einval, "launchctl asuser: bad uid " + user);
            const auto uid = static_cast<std::uint32_t>(std::stoul(user));
            if (uid != environment.user_id && environment.user_id != 0)
                throw ProcessError(eperm, "launchctl asuser " + user + ": not this process's user (" + std::to_string(environment.user_id) + ")");
            path = search_path(argv[3], envp, environment);
            argv.erase(argv.begin(), argv.begin() + 3);
            continue;
        }
        if (is_python_path(path)) {
            const auto alias = python_alias(environment);
            if (!alias) throw ProcessError(enoent, path + ": no guest Python is configured (ALCHEMY_GUEST_PYTHON, or a bundled "
                "<App>.app/Contents/Resources/*/python/bin/python3*)");
            return {"", *alias, path, std::move(argv)};
        }
        const auto host = environment.map_path(path);
        std::string line;
        switch (classify(host, &line)) {
        case FileKind::missing: throw ProcessError(enoent, path + ": no such file");
        case FileKind::directory: throw ProcessError(eacces, path + ": is a directory");
        case FileKind::bundle:
        case FileKind::macho: return {"", host, path, std::move(argv)};
        case FileKind::windows:
            throw ProcessError(enoexec, path + ": is a Windows executable; Alchemy never runs host programs from guest paths");
        case FileKind::other: throw ProcessError(enoexec, path + ": not a Mach-O program or #! script");
        case FileKind::script: {
            std::size_t at = 0;
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
            auto end = at;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
            const auto interpreter = line.substr(at, end - at);
            while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
            auto argument = line.substr(end);
            while (!argument.empty() && (argument.back() == ' ' || argument.back() == '\t')) argument.pop_back();
            if (interpreter.empty()) throw ProcessError(enoexec, path + ": empty #! interpreter");
            std::vector<std::string> next{interpreter};
            if (!argument.empty()) next.push_back(argument);
            next.push_back(path);
            if (argv.size() > 1) next.insert(next.end(), argv.begin() + 1, argv.end());
            argv = std::move(next);
            path = interpreter;
            continue;
        }
        }
    }
    throw ProcessError(enoexec, std::string(guest_path) + ": too many nested #! interpreters");
}

// ---- process creation -------------------------------------------------------------------------
SpawnedProcess spawn_child(const SpawnRequest& request) {
    using libsystem::darwin_spawn_errno;
    struct Owned {
        std::vector<host::Handle> handles;
        ~Owned() { for (const auto handle : handles) host::close(handle); }
    } owned;
    const auto inheritable = [&](host::Handle source) -> host::Handle {
        if (!source) return {};
        const auto copy = host::process::inheritable(source);
        if (!copy) throw ProcessError(darwin_spawn_errno(copy.error()), "cannot pass a descriptor to the child (DuplicateHandle)");
        owned.handles.push_back(*copy);
        return *copy;
    };
    ChildLaunch launch;
    launch.tool = request.target.tool;
    launch.host_program = request.target.host_program;
    launch.guest_program = request.target.guest_program;
    launch.argv = request.target.argv;
    launch.envp = request.envp;
    launch.cwd = request.cwd;
    launch.pid = request.pid; launch.ppid = request.ppid; launch.pgid = request.pgid; launch.sid = request.sid;
    launch.umask = request.umask; launch.ignored_signals = request.ignored_signals; launch.signal_mask = request.signal_mask;
    std::array<host::Handle, 3> standard{};
    std::vector<std::pair<std::size_t, host::Handle>> sockets;
    for (const auto& fd : request.fds) {
        ChildFd entry = fd;
        if (fd.kind == ChildFdKind::standard) {
            using host::process::Standard;
            const auto source = host::process::standard_handle(fd.standard == 0 ? Standard::input : fd.standard == 1 ? Standard::output : Standard::error);
            if (!source) continue;   // no host stream: the descriptor is closed
            entry.kind = ChildFdKind::handle;
            entry.flags = fd.standard == 0 ? 0 : 1;
            entry.handle = inheritable(source).value;
        } else if (fd.kind == ChildFdKind::handle) {
            entry.handle = inheritable(host::Handle{static_cast<std::uintptr_t>(fd.handle)}).value;
        } else if (fd.kind == ChildFdKind::socket) {
            sockets.emplace_back(launch.fds.size(), host::Handle{static_cast<std::uintptr_t>(fd.handle)});
            entry.handle = 0;
        }
        if (entry.kind == ChildFdKind::handle && entry.fd >= 0 && entry.fd < 3)
            standard[static_cast<std::size_t>(entry.fd)] = host::Handle{static_cast<std::uintptr_t>(entry.handle)};
        launch.fds.push_back(std::move(entry));
    }
    // Room for the socket records written once the child's id is known (a
    // host socket export is under a kilobyte).
    auto bytes = encode(launch);
    const auto capacity = bytes.size() + sockets.size() * (1024 + 64) + 4096;
    const auto mapping = host::sections::create_section(capacity, true);
    if (!mapping) throw ProcessError(darwin_spawn_errno(mapping.error()), "cannot create the child launch specification");
    owned.handles.push_back(*mapping);

    host::process::Spawn options;
    const auto self = host::process::image_path(host::process::current_pid());
    if (!self) throw ProcessError(eio, "cannot read the Alchemy executable path");
    options.program = *self;
    options.arguments = request.launch_options;
    {
        std::ostringstream handle_text;
        handle_text << std::hex << mapping->value;
        options.arguments.push_back("--alchemy-child-spec");
        options.arguments.push_back(handle_text.str());
    }
    if (request.target.tool.empty()) {
        options.arguments.push_back("--");
        options.arguments.push_back(native_text(request.target.host_program));
    }
    std::error_code ignored;
    if (!request.cwd.empty() && std::filesystem::is_directory(request.cwd, ignored)) options.working_directory = native_text(request.cwd);
    options.standard_input = standard[0];
    options.standard_output = standard[1];
    options.standard_error = standard[2];
    options.inherit = owned.handles;
    options.new_group = request.new_session || request.new_group;
    options.suspended = true;
    options.end_with_parent = true;   // every child ends when this process does
    const auto spawned = host::process::spawn(options);
    if (!spawned) {
        if (spawned.error() == host::Error::out_of_range) throw ProcessError(e2big, "child command line is too long");
        throw ProcessError(darwin_spawn_errno(spawned.error()), "cannot start the Alchemy child process");
    }
    struct Started {
        const host::process::Child& value; bool keep{};
        ~Started() {
            if (!keep) { static_cast<void>(host::process::terminate(value.process, 1)); host::close(value.process); }
            host::close(value.thread);
        }
    } started{*spawned};
    if (!sockets.empty()) {
        for (const auto& [index, socket] : sockets) {
            auto blob = host::net::export_socket(socket, spawned->pid);
            if (!blob) throw ProcessError(eio, "cannot pass a socket to the child (WSADuplicateSocketW)");
            launch.fds[index].blob = std::move(*blob);
        }
        bytes = encode(launch);
    }
    if (bytes.size() > capacity) throw ProcessError(enomem, "child launch specification overflow");
    {
        const auto view = host::sections::map(*mapping, 0, static_cast<std::size_t>(capacity), true);
        if (!view) throw ProcessError(darwin_spawn_errno(view.error()), "cannot write the child launch specification");
        auto* target = reinterpret_cast<std::uint8_t*>(*view);
        const auto size = static_cast<std::uint64_t>(bytes.size());
        std::memcpy(target, &size, sizeof(size));
        std::memcpy(target + sizeof(size), bytes.data(), bytes.size());
        static_cast<void>(host::sections::unmap(*view));
    }
    if (const auto resumed = host::process::resume(spawned->thread); !resumed)
        throw ProcessError(darwin_spawn_errno(resumed.error()), "cannot resume the child");
    started.keep = true;
    // Diagnostic: ALCHEMY_TRACE_PROCESS=1 logs every child with its argv.
    static const bool trace = [] { const auto value = host::process::environment("ALCHEMY_TRACE_PROCESS"); return value && value->starts_with("1"); }();
    if (trace) {
        std::ostringstream line;
        line << "[alchemy process] child " << spawned->pid << " of " << host::process::current_pid() << ": "
             << (request.target.tool.empty() ? request.target.guest_program : request.target.tool);
        for (const auto& argument : request.target.argv) line << " '" << argument << "'";
        line << " (" << request.fds.size() << " descriptors)\n";
        const auto text = line.str();
        static_cast<void>(host::files::write(host::process::standard_handle(host::process::Standard::error),
                                             std::as_bytes(std::span(text.data(), text.size()))));
    }
    return {reinterpret_cast<void*>(spawned->process.value), spawned->pid};
}

ChildLaunch read_child_launch(std::string_view handle_text) {
    std::uintptr_t value = 0;
    for (const auto c : handle_text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) throw std::runtime_error("invalid --alchemy-child-spec handle");
        value = value * 16 + static_cast<std::uintptr_t>(digit);
    }
    const auto mapping = host::Handle{value};
    // The whole section (size 0); its extent bounds the recorded size.
    const auto view = host::sections::map(mapping, 0, 0, false);
    if (!view) throw std::runtime_error("cannot read the Alchemy child launch specification");
    struct Unmap { std::uintptr_t view; host::Handle mapping; ~Unmap() { static_cast<void>(host::sections::unmap(view)); host::close(mapping); } } unmap{*view, mapping};
    const auto region = host::vm::query(*view);
    std::uint64_t size = 0;
    std::memcpy(&size, reinterpret_cast<const void*>(*view), sizeof(size));
    if (!region || size > region->size - sizeof(size)) throw std::runtime_error("Alchemy child launch specification is truncated");
    return decode(std::span(reinterpret_cast<const std::uint8_t*>(*view) + sizeof(size), static_cast<std::size_t>(size)));
}

// ---- descriptors of the root process ----------------------------------------------------------
ChildFd open_root_descriptor(std::string_view spec) {
    const auto fail = [&](std::string_view why) {
        return std::runtime_error("--fd " + std::string(spec) + ": " + std::string(why));
    };
    const auto equals = spec.find('=');
    const auto colon = spec.find(':', equals == std::string_view::npos ? 0 : equals);
    if (equals == std::string_view::npos || colon == std::string_view::npos) throw fail("expected N=MODE:TARGET");
    int fd = 0;
    const auto number = spec.substr(0, equals);
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), fd);
    if (number.empty() || error != std::errc{} || end != number.data() + number.size())
        throw fail("the descriptor is not a number");
    // 0-2 are the host standard streams (the launcher's own redirections).
    if (fd < 3 || fd >= 4096) throw fail("the descriptor must be 3..4095");
    const auto mode = spec.substr(equals + 1, colon - equals - 1);
    const auto target = spec.substr(colon + 1);
    // Darwin status flags: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 8.
    std::uint64_t flags = 0;
    auto access = host::files::Access::read;
    if (mode == "r") flags = 0;
    else if (mode == "w") { flags = 1; access = host::files::Access::write; }
    else if (mode == "a") { flags = 1 | 8; access = host::files::Access::write; }
    else if (mode == "rw") { flags = 2; access = host::files::Access::read_write; }
    else throw fail("the mode must be r, w, a or rw");
    if (target.empty()) throw fail("no target");
    ChildFd result;
    result.fd = fd; result.kind = ChildFdKind::handle; result.flags = flags;
    if (target.starts_with("handle:")) {
        // A host handle this process inherited: the descriptor takes over a
        // private duplicate, so guest children see it only through their table.
        auto text = target.substr(7);
        int base = 10;
        if (text.starts_with("0x") || text.starts_with("0X")) { text.remove_prefix(2); base = 16; }
        std::uintptr_t value = 0;
        const auto [handle_end, handle_error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
        if (text.empty() || handle_error != std::errc{} || handle_end != text.data() + text.size() || !value)
            throw fail("the handle is not a number");
        const host::Handle inherited{value};
        if (host::files::kind(inherited) == host::files::Kind::unknown) throw fail("not an open file or pipe handle");
        const auto copy = host::duplicate(inherited);
        if (!copy) throw fail("cannot take the handle (" + std::string(host::name(copy.error())) + ")");
        host::close(inherited);
        result.handle = copy->value;
        return result;
    }
    // A host path: a file or a named pipe. Pipes and devices open as they
    // are; a file opened for w is truncated, w/a/rw create a missing file.
    const auto append = (flags & 8) ? host::files::open_append : 0U;
    auto opened = host::files::open(target, access, host::files::Create::open_existing, append);
    if (!opened && opened.error() == host::Error::not_found && mode != "r")
        opened = host::files::open(target, access, host::files::Create::open_or_create, append);
    if (!opened) throw fail("cannot open (" + std::string(host::name(opened.error())) + ")");
    host::OwnedHandle owned(*opened);
    if (host::files::kind(owned.get()) == host::files::Kind::file) {
        if (mode == "w") {
            if (const auto truncated = host::files::set_size(owned.get(), 0); !truncated)
                throw fail("cannot truncate (" + std::string(host::name(truncated.error())) + ")");
        }
        result.path = std::string(target);
    }
    result.handle = owned.release().value;
    return result;
}

} // namespace alchemy
