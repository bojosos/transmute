#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/process_context.hpp"

#include <cstdint>
#include <span>
#include <string>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): Authorization
// Services. An authorization reference can be created, but Alchemy cannot
// launch a privileged helper process, so AuthorizationExecuteWithPrivileges
// fails with errAuthorizationToolExecuteFailure. (Run the process with
// ALCHEMY_SUPERUSER=1, the sudo equivalent, instead.)
namespace alchemy::foundation_shells {

void install_authorization_shell(ProcessContext& process) {
    constexpr std::int32_t success = 0, invalid_set = -60001, tool_execute_failure = -60031;
    const std::string security("/System/Library/Frameworks/Security.framework/Versions/A/Security");
    static_cast<void>(process.install_function("_AuthorizationCreate", [&process](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = static_cast<std::uint32_t>(success); return; }
        const auto reference = process.allocate_guest(16);
        m.fill_bytes(reference, 0, 16);
        m.write64(s.x[3], reference);
        s.x[0] = static_cast<std::uint32_t>(success);
    }, security));
    static_cast<void>(process.install_function("_AuthorizationExecuteWithPrivileges", [&process](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = static_cast<std::uint32_t>(invalid_set); return; }
        bool terminated{};
        const auto tool = m.read_string(s.x[1], 1, 4096, &terminated);
        const std::string text = "Alchemy: AuthorizationExecuteWithPrivileges(" + std::string(tool.begin(), tool.end()) +
            ") is unsupported: no privileged helper process can be launched (set ALCHEMY_SUPERUSER=1 to run as root)\n";
        static_cast<void>(process.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
        s.x[0] = static_cast<std::uint32_t>(tool_execute_failure);
    }, security));
}

} // namespace alchemy::foundation_shells
