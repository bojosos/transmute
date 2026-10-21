#include "alchemy/darwin/foundation/headless_shells.hpp"

// alchemy-fx (darwin/foundation/headless_shells.hpp): cf_preferences.cpp
// registers NSUserDefaults' methods.
namespace alchemy::foundation_shells {

void install_user_defaults_shell(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::foundation_shells
