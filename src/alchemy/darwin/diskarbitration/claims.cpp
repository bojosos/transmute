#include "alchemy/gui/value_provider.hpp"

// DiskArbitration claims: disks cannot be claimed (unsupported) and unclaiming
// is a no-op. Cut from darwin/coreservices/services.cpp (install_services
// calls it in place); Chrome's images first imported them.
namespace alchemy::gui::values {
namespace {

using fill::disk_arbitration;

} // namespace

void Provider::install_disk_claims() {
    unsupported(disk_arbitration, "_DADiskClaim", "disks cannot be claimed under Alchemy");
    raw(disk_arbitration, "_DADiskUnclaim", [](CpuState&, GuestMemory&) {});   // no disk is ever claimed
}

} // namespace alchemy::gui::values
