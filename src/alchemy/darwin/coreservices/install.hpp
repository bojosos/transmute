#pragma once

// CoreServices' install entries (install.cpp). Until the aggregators become
// installer-table rows, each entry is called by the aggregator that installed
// its files, at their old position, so the install order is unchanged:
//   export fill:    install_coreservices_collation_and_types
//   QLab data:      install_coreservices_uttypes
//   values kit:     install_coreservices_system_services

namespace alchemy {
class CoreFoundation;
class ObjcRuntime;
} // namespace alchemy
namespace alchemy::export_fill {
class Provider;
} // namespace alchemy::export_fill
namespace alchemy::gui {
struct InstallContext;
} // namespace alchemy::gui
namespace alchemy::gui::support_kit {
struct Support;
} // namespace alchemy::gui::support_kit
namespace alchemy::gui::values {
class Provider;
} // namespace alchemy::gui::values

namespace alchemy::gui {

void install_coreservices_collation_and_types(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf);  // coreservices.cpp
void install_coreservices_uttypes(support_kit::Support& support);              // uttype_provider.cpp
void install_coreservices_system_services(values::Provider& frameworks);   // services.cpp (and the pieces it calls)
// Its own installer-table row, in the former install_framework_gaps position:
void install_coreservices_carbon_core(InstallContext& context);           // carbon_core.cpp (fp aliases, FSGetVolumeInfo)

} // namespace alchemy::gui
