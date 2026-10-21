#include "alchemy/darwin/diskarbitration/install.hpp"

#include "alchemy/process_context.hpp"

namespace alchemy::diskarbitration {

void install_diskarbitration(gui::InstallContext&) {
    // DiskArbitration: Qt Core and an installer link it without
    // importing any symbol from it (their DiskArbitration paths are unused on
    // these builds). The identity loads; any import of it fails at bind,
    // since no DiskArbitration function is provided.
    ProcessContext::add_provider_library("/System/Library/Frameworks/DiskArbitration.framework/Versions/A/DiskArbitration");
}

} // namespace alchemy::diskarbitration
