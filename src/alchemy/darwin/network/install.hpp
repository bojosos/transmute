#pragma once

// Network's install entries besides install_network (gui/install.hpp, one
// installer-table row). Until the values kit's group becomes table rows, it calls
// this at its old position:
//   values kit:     install_network_framework

namespace alchemy::gui::values {
class Provider;
} // namespace alchemy::gui::values

namespace alchemy::gui {

void install_network_framework(values::Provider& frameworks);   // network_framework.cpp (nw_path_monitor)

} // namespace alchemy::gui
