#include "alchemy/darwin/shells.hpp"

// alchemy-fx (darwin/shells.hpp): darwin/coregraphics/{color,geometry_exports,context_exports,path_exports,
// image,gradient,displays}.cpp and imageio/imageio_exports.cpp register these names.
namespace alchemy::darwin_shells {

void install_coregraphics_shell(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_shells
