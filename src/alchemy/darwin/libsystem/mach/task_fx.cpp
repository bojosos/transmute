#include "alchemy/darwin/libsystem/mach/task_shell.hpp"

namespace alchemy::libsystem {

// alchemy-fx: mach_port_deallocate, task_info, thread_policy_set and
// thread_policy_get are mach/mach_exports.cpp's (gui::install_chrome_support).
void install_mach_task_shell(ProcessContext&) {}

} // namespace alchemy::libsystem
