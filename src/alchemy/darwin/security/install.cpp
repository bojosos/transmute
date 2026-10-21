#include "alchemy/darwin/security/install.hpp"
#include "alchemy/darwin/security/security_internal.hpp"

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/kit/completion/graphics/providers.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

// The Security framework's one entry point (README.md): Security.framework
// (SecTrust, SecKey, keychain, SecureTransport, CommonCrypto; NSError and
// CFError when nothing provides them yet), then GSS.framework. GSS shares no
// name with the providers installed between them before, so the order is
// free (registration audit).
namespace alchemy::gui {

void install_security(InstallContext& context) {
    security::install_security(context);
    install_gss(context);
}

void install_security_common_cryptor(export_fill::Provider& provider) { export_fill::install_cc_cryptor(provider); }
void install_security_cryptokit_host(support_kit::Support& support) { completion_kit::graphics::install_crypto(support); }

} // namespace alchemy::gui
