#include "alchemy/darwin/foundation/headless_shells.hpp"

// alchemy-fx (darwin/foundation/headless_shells.hpp): code_signing.cpp registers
// AuthorizationCreate and AuthorizationExecuteWithPrivileges.
namespace alchemy::foundation_shells {

void install_authorization_shell(ProcessContext&) {}

} // namespace alchemy::foundation_shells
