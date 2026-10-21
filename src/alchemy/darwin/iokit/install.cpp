#include "alchemy/darwin/iokit/devices.hpp"

namespace alchemy::gui::fill {

void install_devices(InstallContext& context) {
    install_iokit_devices(context);
    install_peripherals(context);
}

} // namespace alchemy::gui::fill
