#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/foundation/install.hpp"

// install_completion_kit and the module's diagnostics.
namespace alchemy::gui::completion_kit {

void fail(std::string_view why) { throw std::runtime_error("Alchemy completion kit: " + std::string(why)); }
void unsupported(std::string_view what) { throw std::runtime_error("unsupported: Alchemy completion kit: " + std::string(what)); }

} // namespace alchemy::gui::completion_kit

namespace alchemy::gui {

void install_completion_kit(InstallContext& context) {
    using namespace completion_kit;
    static std::shared_ptr<Support> process_support;
    auto created = std::make_shared<Support>(context.process, context.objc, context.cf);
    process_support = created;
    install_foundation_distributed_notifications(*created);
    alchemy::libsystem::install_os_log(*created);
    install_audio(*created);
    install_foundation_values_and_formats(*created);
    install_disk_arbitration(*created);   // darwin/diskarbitration/sessions.cpp
    install_media_timing(*created);
    install_animator(*created);
    install_foundation_archives_and_data_files(*created);
    install_group_data(*created);
    install_group_runtime(*created);
    install_group_controls(*created);
    install_group_windows(*created);
    install_group_media(*created);
    install_group_graphics(*created);
}

} // namespace alchemy::gui
