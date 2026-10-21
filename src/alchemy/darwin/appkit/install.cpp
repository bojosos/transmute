#include "alchemy/darwin/appkit/appkit_internal.hpp"

// AppKit's installation (install_darwin_gui) and the process's AppKit
// instance. The providers are appkit.cpp and the files it lists.
namespace alchemy::gui {
namespace appkit {

namespace {
std::weak_ptr<AppKit> process_instance;
std::mutex process_instance_mutex;
} // namespace

std::shared_ptr<AppKit> instance() noexcept {
    std::lock_guard lock(process_instance_mutex);
    return process_instance.lock();
}

} // namespace appkit

void install_appkit_gui(InstallContext& context) {
    // Registration only: classes, methods and exports. NSApp, windows and
    // every window-server resource are created when the guest asks.
    const auto kit = std::make_shared<appkit::AppKit>(context.process, context.objc, context.cf);
    {
        std::lock_guard lock(appkit::process_instance_mutex);
        appkit::process_instance = kit;
    }
    kit->install();
}

} // namespace alchemy::gui
