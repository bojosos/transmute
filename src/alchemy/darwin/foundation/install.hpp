#pragma once

// Foundation's install entries (install.cpp). The framework's headless part
// (foundation_bridge.cpp, foundation_extra.cpp) is installed with the process
// (FoundationBridge). Its GUI-set files are installed through these entries,
// and until the aggregators become installer-table rows, each entry is called
// by the aggregator that installed its files, at their old position, so the
// install order is unchanged:
//   support kit:   install_foundation_associated_objects,
//                  install_foundation_block_observers,
//                  install_foundation_mutable_strings
//   export fill:   install_foundation_value_classes
//   completion kit:install_foundation_distributed_notifications,
//                  install_foundation_values_and_formats,
//                  install_foundation_archives_and_data_files
//   QLab data:     install_foundation_data_classes,
//                  install_foundation_xml_urls_coding,
//                  install_foundation_archive_classes
//   QLab runtime:  install_foundation_exceptions,
//                  install_foundation_invocations_undo_kvo,
//                  install_foundation_kvc_collections_and_queues,
//                  install_foundation_files_and_bundles,
//                  install_foundation_scripting
//   fwgaps:        install_foundation_property_list_serialization

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

namespace alchemy::gui {

void install_foundation_associated_objects(support_kit::Support& support);       // objc_assoc.cpp (libobjc's, here for now)
void install_foundation_block_observers(support_kit::Support& support);          // notification_blocks.cpp
void install_foundation_mutable_strings(support_kit::Support& support);          // mutable_string.cpp
void install_foundation_value_classes(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf);   // value_classes.cpp
// Its own installer-table row, in the former install_framework_gaps position:
void install_foundation_file_attribute_keys(InstallContext& context);   // file_attribute_keys.cpp
void install_foundation_distributed_notifications(support_kit::Support& support);  // distributed_notifications.cpp
void install_foundation_values_and_formats(support_kit::Support& support);       // kvc, collections, formatters, json_serialization, constants, file_attributes, immutable_copies
void install_foundation_archives_and_data_files(support_kit::Support& support);  // keyed_archive.cpp, data_files.cpp
void install_foundation_data_classes(support_kit::Support& support);             // class_objects ... data_extra
void install_foundation_xml_urls_coding(support_kit::Support& support);          // xml_provider, url_extra, coding
void install_foundation_archive_classes(support_kit::Support& support);          // archives_provider.cpp
void install_foundation_exceptions(support_kit::Support& support);               // exceptions.cpp
void install_foundation_invocations_undo_kvo(support_kit::Support& support);     // invocation.cpp, undo.cpp, kvo.cpp
void install_foundation_kvc_collections_and_queues(support_kit::Support& support);  // kvc_collections.cpp, notification_queue.cpp
void install_foundation_files_and_bundles(support_kit::Support& support);        // files.cpp, bundles.cpp
void install_foundation_scripting(support_kit::Support& support);                // scripting.cpp
void install_foundation_property_list_serialization(InstallContext& context);  // property_list_serialization.cpp

} // namespace alchemy::gui
