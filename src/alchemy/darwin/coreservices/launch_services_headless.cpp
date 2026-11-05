#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/process_context.hpp"

#include <string>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): a headless process
// has no application registry, so LSCopyDefaultApplicationURLForContentType
// finds no application (NULL, no error object).
namespace alchemy::foundation_shells {

void install_launch_services_shell(ProcessContext& process) {
    static_cast<void>(process.install_function("_LSCopyDefaultApplicationURLForContentType", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = 0;
    }, "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices"));
}

} // namespace alchemy::foundation_shells
