#pragma once

// Security's install entries besides install_security (gui/install.hpp, one
// installer-table row). Until the aggregators become table rows, each is
// called by the aggregator that installed its files, at their old position:
//   export fill (libSystem): install_security_common_cryptor
//   QLab graphics:           install_security_cryptokit_host
// Chrome's legacy keychain (legacy_keychain.cpp) comes with
// install_coreservices_system_services (darwin/coreservices/install.hpp).

namespace alchemy::export_fill {
class Provider;
} // namespace alchemy::export_fill
namespace alchemy::gui::support_kit {
struct Support;
} // namespace alchemy::gui::support_kit

namespace alchemy::gui {

void install_security_common_cryptor(export_fill::Provider& provider);   // cc_cryptor.cpp
void install_security_cryptokit_host(support_kit::Support& support);    // crypto_exports.cpp (the CryptoKit overlay's host primitives)

} // namespace alchemy::gui
