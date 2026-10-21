#pragma once

#include "alchemy/gui/install.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"

// libdispatch's rows of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only). The provider itself (dispatch.cpp) is installed by
// alchemy::DarwinProviders.
namespace alchemy::dispatch {

// dispatch_data_t (dispatch_data.cpp; fill-only), last in the position of the
// former gap-fill group. Dispatch objects as Objective-C objects
// and I/O channels are installed with the QLab runtime group
// (dispatch_objects.cpp).
void install_dispatch_data(gui::InstallContext& context);

// Installed by the shared QLab kit's runtime aggregator, in its order:
using gui::completion_kit::install_dispatch_objects;  // dispatch_objects.cpp (kit/completion/runtime/install.cpp)

} // namespace alchemy::dispatch
