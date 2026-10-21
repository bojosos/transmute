#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

// QuartzCore's installation (install_darwin_gui). The provider object is
// quartzcore.cpp's (quartzcore::install_provider).
namespace alchemy::gui {

void install_quartzcore(InstallContext& context) {
    // Installation only registers methods; no device, window or thread is
    // created until a layer presents (doctor and Blender -b stay headless);
    // only the present watcher (ALCHEMY_PRESENT_CAPTURE/_STATS) starts here,
    // so the stats file exists even when nothing ever presents.
    quartzcore::start_present_services();
    quartzcore::install_provider(context);
}

} // namespace alchemy::gui
