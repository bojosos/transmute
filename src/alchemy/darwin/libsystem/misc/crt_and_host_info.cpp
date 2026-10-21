#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include "alchemy/host/system.hpp"

#include <mutex>
#include <thread>
#include <vector>

// libSystem (headless-correct): _NSGetArgc/_NSGetArgv over the startup vector,
// getchar, wcsstr and wcspbrk (32-bit wchar_t), host_info(HOST_BASIC_INFO)
// from the host's processors and memory.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class CrtAndHostInfo final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    Address argc_cell{}, argv_cell{};

    void install() {
        // _NSGetArgc/_NSGetArgv (crt_externs.h): cells holding the argc/argv
        // main received. Programs walk past argv's NULL into envp and the
        // apple[] vector (executable_path=...), so the startup vector itself
        // is published, not a copy.
        const auto vector = [this] {
            std::lock_guard lock(mutex);
            if (argv_cell) return;
            if (!context.startup_argv) throw std::runtime_error("Alchemy libSystem: _NSGetArgv before process startup");
            argc_cell = context.allocate_guest(8);
            memory().write64(argc_cell, context.startup_argc);
            argv_cell = context.allocate_guest(8);
            memory().write64(argv_cell, context.startup_argv);
        };
        function(libsystem, "__NSGetArgc", [this, vector](CpuState& s, GuestMemory&) { vector(); s.x[0] = argc_cell; });
        function(libsystem, "__NSGetArgv", [this, vector](CpuState& s, GuestMemory&) { vector(); s.x[0] = argv_cell; });
        function(libsystem, "_getchar", [this](CpuState& s, GuestMemory& m) {
            const auto stdin_cell = context.resolve(libsystem, "___stdinp");
            if (!stdin_cell) throw std::runtime_error("Alchemy libSystem: getchar needs ___stdinp");
            s.x[0] = guest_call(libsystem, "_fgetc", {m.read64(stdin_cell->address)});
        });
        // wchar_t is 32-bit on Darwin.
        const auto wide = [](GuestMemory& m, Address address) {
            std::vector<std::uint32_t> text;
            for (Address at = address;; at += 4) {
                const auto value = m.read32(at);
                if (!value) break;
                if (text.size() > (64U << 20)) throw std::runtime_error("Alchemy libSystem: wide string exceeds 64 Mi characters");
                text.push_back(value);
            }
            return text;
        };
        function(libsystem, "_wcsstr", [wide](CpuState& s, GuestMemory& m) {
            const auto haystack = wide(m, s.x[0]), needle = wide(m, s.x[1]);
            if (needle.empty()) return;
            const auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
            s.x[0] = found == haystack.end() ? 0 : s.x[0] + static_cast<Address>(found - haystack.begin()) * 4;
        });
        function(libsystem, "_wcspbrk", [wide](CpuState& s, GuestMemory& m) {
            const auto text = wide(m, s.x[0]), set = wide(m, s.x[1]);
            const auto found = std::find_first_of(text.begin(), text.end(), set.begin(), set.end());
            s.x[0] = found == text.end() ? 0 : s.x[0] + static_cast<Address>(found - text.begin()) * 4;
        });
        // proc_pidpath: DarwinServices (this process, its children and the
        // rest of the Alchemy process tree).
        // host_info(HOST_BASIC_INFO) from the host's processors and memory.
        function(libsystem, "_host_info", [](CpuState& s, GuestMemory& m) {
            constexpr std::uint32_t host_basic_info = 1, basic_count = 12, kern_invalid_argument = 4, kern_failure = 5;
            if (s.x[1] != host_basic_info) { s.x[0] = kern_invalid_argument; return; }
            if (!s.x[2] || !s.x[3] || m.read32(s.x[3]) < basic_count) { s.x[0] = kern_failure; return; }
            const auto cpus = std::max(1U, std::thread::hardware_concurrency());
            std::uint64_t bytes = 8ULL << 30;
            if (const auto memory = host::system::memory()) bytes = memory->physical;
            const std::uint32_t fields[] = {cpus, cpus, static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes, 0x80000000ULL)),
                0x0100000cU, 0, 0, cpus, cpus, cpus, cpus};
            for (std::size_t index = 0; index < std::size(fields); ++index) m.write32(s.x[2] + index * 4, fields[index]);
            m.write64(s.x[2] + 40, bytes);
            m.write32(s.x[3], basic_count);
            s.x[0] = 0;
        });
        // tcgetattr, tcsetattr: libsystem/fs/terminal.cpp.
    }
};

} // namespace

void install_libsystem_crt_and_host_info(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<CrtAndHostInfo>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
