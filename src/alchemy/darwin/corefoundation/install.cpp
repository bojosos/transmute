#include "alchemy/darwin/corefoundation/install.hpp"

#include "alchemy/gui/value_provider.hpp"
#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"
#include "alchemy/darwin/kit/completion/runtime/common.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

// CoreFoundation's install entries (install.hpp), each the files of one old
// aggregator position, in their old order.
namespace alchemy::gui {

namespace fill {
void install_cf_collections(InstallContext& context);   // cf_collections.cpp
void install_cf_urls(InstallContext& context);          // cf_urls.cpp
} // namespace fill

void install_corefoundation_additions(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf) {
    export_fill::install_cf_extra(provider, objc, cf);
    export_fill::install_cf_system(provider, cf);
    export_fill::install_cf_preferences(provider, objc, cf);
    export_fill::install_cf_locale(provider, objc, cf);
}

void install_corefoundation_file_descriptors(support_kit::Support& support) { completion_kit::install_cf_extras(support); }

void install_corefoundation_sets_and_charsets(InstallContext& context) {
    fill::install_cf_gaps(context);
    fill::install_character_sets(context);
    fill::install_cf_collections(context);
    fill::install_cf_urls(context);
}

void install_corefoundation_string_clusters(values::Provider& frameworks) { frameworks.install_foundation(); }

} // namespace alchemy::gui
