#include "alchemy/darwin/shells.hpp"

// alchemy-fx (darwin/shells.hpp): darwin/coretext/{attributed,font_exports,typesetter}.cpp register these
// names.
namespace alchemy::darwin_shells {

void install_coretext_shell(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_shells
