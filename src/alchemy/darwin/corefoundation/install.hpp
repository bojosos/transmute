#pragma once

// CoreFoundation's install entries (install.cpp). The object store and the
// CoreFoundation provider are installed with the process (CoreFoundation).
// Its GUI-set files are installed through these entries, and until the
// aggregators become installer-table rows, each entry is called by the
// aggregator that installed its files, at their old position, so the install
// order is unchanged:
//   export fill:    install_corefoundation_additions
//   QLab runtime:   install_corefoundation_file_descriptors
//   fwgaps:         install_corefoundation_sets_and_charsets
//   values kit:     install_corefoundation_string_clusters

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

void install_corefoundation_additions(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf);  // cf_extra, cf_preferences, cf_locale
void install_corefoundation_file_descriptors(support_kit::Support& support);   // cf_file_descriptors.cpp (and libSystem's fts_err.cpp)
void install_corefoundation_sets_and_charsets(InstallContext& context);      // cf_sets_notifications.cpp, charset.cpp
void install_corefoundation_string_clusters(values::Provider& frameworks);  // cf_string_clusters.cpp
// Its own installer-table row, in the former install_framework_gaps position:
void install_corefoundation_time_zones_and_uuids(InstallContext& context);  // time_zones_and_uuids.cpp

} // namespace alchemy::gui
