#pragma once

// Device frameworks for strict mode (src/alchemy/devices): IOKit HID, power
// and notification gaps, ForceFeedback, DiskArbitration, IOBluetooth,
// CoreBluetooth, GameController, CoreWLAN, CoreLocation and
// LocalAuthentication. Qt WebEngine links them; installed by install_devices
// (fill-only). Alchemy exposes no HID, Bluetooth, game-controller, Wi-Fi,
// location or biometric hardware: every query answers "none" or
// "unsupported" with the framework's documented values.

#include "alchemy/gui/fill_provider.hpp"

namespace alchemy::gui::fill {

void install_iokit_devices(InstallContext& context);
void install_peripherals(InstallContext& context);

} // namespace alchemy::gui::fill
