#include "alchemy/darwin/network/install.hpp"
#include "alchemy/darwin/network/network_internal.hpp"

#include "alchemy/gui/value_provider.hpp"

#include "alchemy/gui/install.hpp"

// The network provider's one entry point (install_network). Identity:
// /System/Library/Frameworks/CFNetwork.framework/Versions/A/CFNetwork
// (listed in darwin_framework_libraries); the Foundation classes are
// published under Foundation's install name. Requires install_security
// (bridge, SecTrust) and runs after it. Creates classes and constant
// strings only: WinHTTP sessions are opened when a task first runs.
namespace alchemy::gui {

void install_network(InstallContext& context) {
    static_cast<void>(context);
    static std::shared_ptr<network::Urls> urls;
    urls = std::make_shared<network::Urls>(security::bridge());
    network::install_urls(*urls);
    network::install_operation_queues(*urls->b);
    network::install_url_loading(urls);
    network::install_cookies(urls);
    network::install_cfnetwork(urls);
    network::install_pac(urls);
    network::install_sockets(urls->b);
    network::install_system_configuration_store(urls->b);
    network::install_resolver(urls->b);
    network::install_dns_sd(urls->b);
    network::install_net_services(urls->b);
    network::install_network_connections(urls->b);
}

void install_network_framework(values::Provider& frameworks) { frameworks.install_network(); }

} // namespace alchemy::gui
