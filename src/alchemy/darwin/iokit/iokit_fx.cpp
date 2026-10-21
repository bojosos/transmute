#include "alchemy/darwin/shells.hpp"

// alchemy-fx (darwin/shells.hpp): darwin/iokit/iokit_registry.cpp registers these names.
namespace alchemy::darwin_shells {

void install_iokit_shell(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_shells
