#include "alchemy/darwin/foundation/install.hpp"

#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"
#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/kit/support/support_kit.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

// Foundation's install entries (install.hpp), each the files of one old
// aggregator position, in their old order.
namespace alchemy::gui::completion_kit {
void install_streams(Support& support);          // streams.cpp
void install_url_components(Support& support);   // url_components.cpp
} // namespace alchemy::gui::completion_kit
namespace alchemy::gui::completion_kit::data {
void install_strings_more(Support& support);       // strings_more.cpp
void install_collections_more(Support& support);   // collections_more.cpp
void install_foundation_more(Support& support);    // foundation_more.cpp
} // namespace alchemy::gui::completion_kit::data

namespace alchemy::gui {

void install_foundation_associated_objects(support_kit::Support& support) { support_kit::install_objc_associations(support); }
void install_foundation_block_observers(support_kit::Support& support) { support_kit::install_notification_blocks(support); }
void install_foundation_mutable_strings(support_kit::Support& support) { support_kit::install_mutable_string(support); }

void install_foundation_value_classes(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf) {
    export_fill::install_foundation_classes(provider, objc, cf);
}

void install_foundation_distributed_notifications(support_kit::Support& support) { completion_kit::install_foundation(support); }

void install_foundation_values_and_formats(support_kit::Support& support) {
    completion_kit::install_key_value_coding(support);
    completion_kit::install_collections(support);
    completion_kit::install_formatters(support);
    completion_kit::install_json_serialization(support);
    completion_kit::install_constants(support);
    completion_kit::install_file_attributes(support);
    completion_kit::install_immutable_copies(support);
}

void install_foundation_archives_and_data_files(support_kit::Support& support) {
    completion_kit::install_keyed_archive(support);
    completion_kit::install_data_files(support);
}

void install_foundation_data_classes(support_kit::Support& support) {
    completion_kit::data::install_class_objects(support);
    completion_kit::data::install_sorting(support);
    completion_kit::data::install_collections_extra(support);
    completion_kit::data::install_collections_more(support);
    completion_kit::data::install_pointer_collections(support);
    completion_kit::data::install_strings(support);
    completion_kit::data::install_strings_more(support);
    completion_kit::data::install_scanner(support);
    completion_kit::data::install_regex(support);
    completion_kit::data::install_predicates(support);
    completion_kit::data::install_decimals(support);
    completion_kit::data::install_calendars(support);
    completion_kit::data::install_formatters_extra(support);
    completion_kit::data::install_data_extra(support);
}

void install_foundation_xml_urls_coding(support_kit::Support& support) {
    completion_kit::data::install_xml(support);
    completion_kit::data::install_url_extra(support);
    completion_kit::data::install_coding(support);
}

void install_foundation_archive_classes(support_kit::Support& support) { completion_kit::data::install_archives(support); }

void install_foundation_exceptions(support_kit::Support& support) { completion_kit::install_exceptions(support); }

void install_foundation_invocations_undo_kvo(support_kit::Support& support) {
    completion_kit::install_invocations(support);
    completion_kit::install_undo_manager(support);
    completion_kit::install_key_value_observing(support);
}

void install_foundation_kvc_collections_and_queues(support_kit::Support& support) {
    completion_kit::install_kvc_collections(support);
    completion_kit::install_notification_queue(support);
    completion_kit::install_value_transformers(support);
}

void install_foundation_files_and_bundles(support_kit::Support& support) {
    completion_kit::install_streams(support);
    completion_kit::install_url_components(support);
    completion_kit::install_files(support);
    completion_kit::data::install_foundation_more(support);
    completion_kit::install_bundles(support);
}

void install_foundation_scripting(support_kit::Support& support) { completion_kit::install_scripting(support); }

void install_foundation_property_list_serialization(InstallContext& context) { fill::install_foundation_gaps(context); }

} // namespace alchemy::gui
