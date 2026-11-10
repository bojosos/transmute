// linux backend of fx/host/process.hpp.
#include "fx/host/process.hpp"
#include "fx/host/linux/internal.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace transmute::fx::host {

namespace {

std::atomic<bool> g_shutting_down{false};
const bool g_registered = [] {
    std::atexit([] { g_shutting_down.store(true, std::memory_order_release); });
    return true;
}();

// Splits a Windows-style command line (CommandLineToArgvW rules for quotes
// and backslashes) into arguments.
std::vector<std::string> split_command_line(const std::string& line) {
    std::vector<std::string> arguments;
    std::string current;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\') {
            std::size_t slashes = 0;
            while (i < line.size() && line[i] == '\\') { ++slashes; ++i; }
            if (i < line.size() && line[i] == '"') {
                current.append(slashes / 2, '\\');
                if (slashes % 2) { current += '"'; any = true; }
                else { quoted = !quoted; any = true; }
            } else {
                current.append(slashes, '\\');
                --i;
            }
            any = true;
        } else if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if ((c == ' ' || c == '\t') && !quoted) {
            if (any) arguments.push_back(current);
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (any) arguments.push_back(current);
    return arguments;
}

} // namespace

std::uint32_t process_id() noexcept { return static_cast<std::uint32_t>(getpid()); }

double process_age_ms() noexcept {
    ProcessTimes times;
    if (!process_times(times) || !times.created) return 0;
    return static_cast<double>(wall_time() - times.created) / 1e4;
}

std::wstring executable_path() {
    char self[4096];
    const auto length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) return {};
    return linux_internal::widen(std::string(self, static_cast<std::size_t>(length)));
}

bool process_qos_explicit() noexcept { return false; }
void set_process_qos(bool) noexcept {}
void set_process_idle_priority() noexcept { setpriority(PRIO_PROCESS, 0, 19); }
void set_process_below_normal_priority() noexcept { setpriority(PRIO_PROCESS, 0, 5); }

bool process_shutting_down() noexcept {
    static_cast<void>(g_registered);
    return g_shutting_down.load(std::memory_order_acquire);
}

void terminate_process(std::uint32_t exit_code) noexcept { _exit(static_cast<int>(exit_code)); }

bool spawn_background_process(const std::wstring& program, const std::wstring& command_line,
                              void (*on_exit)(void* context), void* context) {
    const auto path = linux_internal::narrow(program);
    auto arguments = split_command_line(linux_internal::narrow(command_line));
    if (arguments.empty()) arguments.push_back(path);
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    // No inherited descriptors besides stdio's replacement, a session of its
    // own (no terminal signals), idle priority (set by the helper itself).
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    pid_t child = 0;
    const int error = posix_spawn(&child, path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) return false;
    setpriority(PRIO_PROCESS, static_cast<id_t>(child), 19);
    if (!on_exit) return true;   // reaped by nobody: init adopts it when we exit
    try {
        std::thread([child, on_exit, context] {
            int status = 0;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            on_exit(context);
        }).detach();
    } catch (...) {
    }
    return true;
}

} // namespace transmute::fx::host
