#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/objc_runtime.hpp"

#include <stdexcept>

// alchemy.exe only (headless_shells.hpp): block observers are unsupported.
namespace alchemy::foundation_shells {

void install_notification_block_shell(ObjcRuntime& objc, std::uint64_t center_class) {
    static_cast<void>(objc.add_native_method(center_class, "addObserverForName:object:queue:usingBlock:",
        [](transmute::CpuState&, transmute::GuestMemory&) {
            throw std::runtime_error("Alchemy Foundation: block-based notification observers are unsupported");
        }));
}

} // namespace alchemy::foundation_shells
