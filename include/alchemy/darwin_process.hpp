#pragma once

// Darwin child processes on a Windows host (docs/ALCHEMY_PROCESS.md).
//
// Every guest child is a new instance of the running Alchemy executable
// (GetModuleFileName): a Mach-O target runs as a guest program, the built-in
// tools (sh, the /usr/bin/env resolution) run without guest code. The parent
// passes the child's argv, envp, working directory, identity and descriptor
// table in a read-only "launch specification" (an inheritable file mapping
// named on the child's command line); descriptors travel as inheritable
// Windows handles restricted with PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Children
// run in a kill-on-close job object of their parent, so no child outlives it.
//
// The guest-facing libSystem surface (pipe, fork/vfork emulation,
// posix_spawn, exec*, wait*, kill, popen, system) lives in darwin_services;
// this header is the host machinery it shares with alchemy's main().

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

// A child exits with this value | signal when a Darwin signal's default action
// ended it (kill() from its parent, or a fatal guest signal), so waitpid can
// report WIFSIGNALED. Top-level processes keep the shell status 128 + signal.
inline constexpr std::uint32_t child_signal_exit_marker = 0x41430000U;

// Darwin <sys/wait.h> status word for a Windows exit code of an Alchemy child.
[[nodiscard]] std::uint32_t darwin_wait_status(std::uint32_t exit_code) noexcept;

// One descriptor of a child's table.
enum class ChildFdKind : std::uint32_t {
    handle = 1,     // file, pipe, character device: an inheritable Windows handle
    directory = 2,  // O_DIRECTORY descriptor: reopened from `path`
    random = 3,     // /dev/urandom (Alchemy's virtual device)
    standard = 4,   // the parent's own host standard stream `standard`
    socket = 5,     // WSADuplicateSocketW protocol information in `blob`
};
struct ChildFd {
    int fd{};
    ChildFdKind kind{ChildFdKind::handle};
    std::uint64_t handle{};      // inheritable handle value (kind handle) as seen by the child
    std::uint64_t flags{};       // Darwin open/status flags (O_ACCMODE, O_APPEND, O_NONBLOCK)
    int standard{-1};            // kind standard: 0, 1 or 2
    std::string path;            // UTF-8 host path (file, directory)
    std::vector<std::uint8_t> blob;
    std::int32_t socket_domain{}, socket_type{}, socket_protocol{};
    // kind socket: a pseudo-terminal end, /dev/ttys<terminal - 1> (0: none).
    std::uint32_t terminal{};
    bool terminal_master{};
};

// Decoded launch specification of an Alchemy child.
struct ChildLaunch {
    std::string tool;                    // "" (Mach-O program) or "sh"
    std::filesystem::path host_program;  // Mach-O file or .app bundle
    std::string guest_program;           // the path exec'd (apple executable_path)
    std::vector<std::string> argv, envp;
    std::filesystem::path cwd;           // host working directory
    std::uint32_t pid{}, ppid{}, pgid{}, sid{};
    std::uint32_t umask{022};
    std::uint32_t ignored_signals{};     // SIG_IGN dispositions that survive exec
    std::uint32_t signal_mask{};
    std::vector<ChildFd> fds;            // 0..2 included unless closed
};

// Error from process creation: a Darwin errno and a diagnostic.
class ProcessError : public std::runtime_error {
public:
    ProcessError(std::uint32_t error, const std::string& message) : std::runtime_error(message), error_(error) {}
    [[nodiscard]] std::uint32_t error() const noexcept { return error_; }
private:
    std::uint32_t error_;
};

// ---- target resolution -------------------------------------------------------
// What an exec of a guest path runs. Mach-O files and .app bundles become an
// Alchemy child; "#!" scripts run their interpreter (sh is the built-in shell,
// /usr/bin/env searches PATH, python3 maps to the configured guest Python);
// Windows executables and other files fail with ENOEXEC.
struct ExecTarget {
    std::string tool;                    // "" or "sh"
    std::filesystem::path host_program;
    std::string guest_program;
    std::vector<std::string> argv;
};
struct ExecEnvironment {
    // Maps a guest path (absolute or relative to the caller's directory) to a host path.
    std::function<std::filesystem::path(std::string_view)> map_path;
    std::filesystem::path main_executable;   // host path of the running program (Python alias)
    std::vector<std::string> host_environment;  // Alchemy's own environment (ALCHEMY_GUEST_PYTHON)
    std::uint32_t user_id{1000};             // the caller's effective uid (launchctl asuser)
};
// envp may be rewritten (env NAME=VALUE assignments).
[[nodiscard]] ExecTarget resolve_exec(std::string_view guest_path, std::vector<std::string> argv,
                                      std::vector<std::string>& envp, const ExecEnvironment& environment);
// PATH search as execvp/posix_spawnp do (PATH from envp, default /usr/bin:/bin).
// Returns the first candidate that resolves, else throws ProcessError (ENOENT/EACCES).
[[nodiscard]] std::string search_path(std::string_view file, const std::vector<std::string>& envp,
                                      const ExecEnvironment& environment);
// True when the guest path names something resolve_exec can run (access X_OK).
[[nodiscard]] bool guest_executable(std::string_view guest_path, const ExecEnvironment& environment);
// True for the programs that exist only for exec: /bin/sh (and bash, zsh,
// dash), /usr/bin/env, and python3 aliases when a guest Python is known.
[[nodiscard]] bool virtual_program(std::string_view guest_path, const ExecEnvironment& environment);
// Default guest PATH replacing a Windows-style host PATH.
inline constexpr std::string_view default_guest_path = "/usr/bin:/bin:/usr/sbin:/sbin";

// ---- process creation ------------------------------------------------------------
struct SpawnRequest {
    ExecTarget target;
    std::vector<std::string> envp;
    std::filesystem::path cwd;
    std::vector<ChildFd> fds;                 // handles here are the parent's (not yet inheritable)
    std::uint32_t pid{}, ppid{}, pgid{}, sid{};  // pid 0: the child's own Windows id
    bool new_session{}, new_group{};
    std::uint32_t umask{022}, ignored_signals{}, signal_mask{};
    std::vector<std::string> launch_options;  // runtime options of the Alchemy command line
};
struct SpawnedProcess {
    void* process{};       // Windows process HANDLE owned by the caller
    std::uint32_t pid{};   // Windows process id
};
// Starts a child; throws ProcessError. Waits for nothing.
[[nodiscard]] SpawnedProcess spawn_child(const SpawnRequest& request);

// ---- child side ---------------------------------------------------------------------
// Parses "--alchemy-child-spec <hex handle>" input, maps and closes it.
[[nodiscard]] ChildLaunch read_child_launch(std::string_view handle_text);
// Runs a built-in tool child (no guest code). Returns its exit code.
[[nodiscard]] int run_child_tool(const ChildLaunch& launch, const std::vector<std::string>& launch_options);

// ---- descriptors of the root process ---------------------------------------------------------
// The launcher's `--fd N=MODE:TARGET` (the analogue of a shell's N< N> N>> N<>
// redirections): a host object the top-level guest process starts with as
// descriptor N (3..4095), described as a child's descriptor is. MODE is r
// (O_RDONLY), w (O_WRONLY; a file is created or truncated), a (O_WRONLY |
// O_APPEND; created) or rw (O_RDWR; created). TARGET is `handle:VALUE`, a host
// handle (decimal or 0x hex) this process inherited, which the descriptor takes
// over, or a host path: a file, a named pipe or a device. Throws
// std::runtime_error for a bad spec or a target that does not open.
[[nodiscard]] ChildFd open_root_descriptor(std::string_view spec);

// ---- shell -----------------------------------------------------------------------------
// The built-in POSIX shell subset used for /bin/sh, system() and popen():
// simple commands with quoting, parameter expansion ($VAR ${VAR} $? $$ $# $@ $*
// $0-$9), NAME=VALUE prefixes, redirections (< > >> <> N>&M N<&M N>&-), pipelines,
// ; && || lists, and the builtins : true false exit cd echo export unset exec.
// Anything else (control structures, functions, command substitution,
// globbing, here-documents, background jobs, subshells) fails explicitly with
// exit status 2. Exposed for tests: parse and report.
[[nodiscard]] std::optional<std::string> shell_syntax_error(std::string_view script);

} // namespace alchemy
