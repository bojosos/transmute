#include "alchemy/darwin/headless_additions.hpp"

// alchemy-fx: OpenAL is the real mixer's (mixer.cpp, installer row
// install_openal_gui); the headless answer (install.cpp) is alchemy.exe's only.
namespace alchemy::darwin_headless {

void install_openal(ProcessContext&, ObjcRuntime&, CoreFoundation&) {}

} // namespace alchemy::darwin_headless
