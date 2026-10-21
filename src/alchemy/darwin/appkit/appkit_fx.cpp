#include "alchemy/darwin/shells.hpp"

// alchemy-fx (darwin/shells.hpp): darwin/appkit/{carbon,hitoolbox,pasteboard,panels,graphics}.cpp and
// darwin/coregraphics/appkit_drawing.cpp register these names.
namespace alchemy::darwin_shells {

void install_appkit_shell(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_shells
