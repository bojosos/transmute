#pragma once

#include "alchemy/gui/install.hpp"

// DiskArbitration's row of the GUI installer table (src/alchemy/gui/install.cpp;
// alchemy-fx only). Its other pieces are installed by the aggregators that
// installed them before, at their old position:
//   completion kit:gui::completion_kit::install_disk_arbitration (sessions.cpp)
//   values kit:    values::Provider::install_disk_claims (claims.cpp, called by
//                  darwin/coreservices/services.cpp in place)
//   fill::install_devices (darwin/iokit/): kDADiskDescriptionMatchVolumeMountable
//                  and kDADiskDescriptionWatchVolumePath
namespace alchemy::diskarbitration {

// The identity (install.cpp).
void install_diskarbitration(gui::InstallContext& context);

} // namespace alchemy::diskarbitration
