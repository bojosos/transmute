#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/core_foundation.hpp"
#include "alchemy/placeholder_exports.hpp"
#include "alchemy/process_context.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): CFSocket binds, and
// a call stops the process. The exports are placeholders, so a provider that
// implements CFSocket may replace them.
namespace alchemy::foundation_shells {

void install_cfsocket_shell(ProcessContext& process) {
    for (const std::string_view symbol : {"_CFSocketCreateRunLoopSource", "_CFSocketCreateWithNative", "_CFSocketGetSocketFlags",
                                          "_CFSocketInvalidate", "_CFSocketSetSocketFlags"}) {
        auto message = "Alchemy: " + std::string(symbol.substr(1)) + " is unsupported in a headless process (CFSocket is not implemented)";
        static_cast<void>(process.install_function(std::string(symbol),
            [message = std::move(message)](transmute::CpuState&, transmute::GuestMemory&) { throw std::runtime_error(message); },
            std::string(core_foundation_library)));
        mark_placeholder_export(process, core_foundation_library, symbol);
    }
}

} // namespace alchemy::foundation_shells
