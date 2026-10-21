#include "alchemy/darwin/foundation/headless_shells.hpp"

// alchemy-fx (darwin/foundation/headless_shells.hpp): timers.cpp registers
// NSRunLoop's run loop methods.
namespace alchemy::foundation_shells {

void install_run_loop_shell(ObjcRuntime&, ProcessContext&, std::uint64_t, std::function<std::uint64_t(std::uint64_t)>) {}

} // namespace alchemy::foundation_shells
