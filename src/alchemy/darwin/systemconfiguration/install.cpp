#include "alchemy/darwin/systemconfiguration/system_configuration.hpp"

#include "alchemy/gui/install.hpp"

// The SystemConfiguration framework's one entry point (README.md): the proxy
// settings. The dynamic store and reachability (sysconfig_store.cpp) need
// the network provider's bridge, so install_network installs them.
namespace alchemy::gui {

void install_system_configuration(InstallContext& context) { sysconfig::install_proxies(context); }

} // namespace alchemy::gui
