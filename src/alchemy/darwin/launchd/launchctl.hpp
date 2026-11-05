#pragma once

// /bin/launchctl under Alchemy: a built-in tool child (like sh), talking to
// the session launchd (client.hpp). `asuser` is resolved before this runs
// (resolve_exec execs the program itself).

#include "alchemy/darwin_process.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::launchd {

// Runs launchctl with launch.argv; `output(fd, text)` writes to the tool's
// descriptor 1 or 2. Returns the exit status.
int launchctl_main(const ChildLaunch& launch, const std::vector<std::string>& launch_options,
                   const std::function<void(int, std::string_view)>& output);

} // namespace alchemy::launchd
