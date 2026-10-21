#include "alchemy/darwin/coreservices/install.hpp"

#include "alchemy/gui/value_provider.hpp"
#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

// CoreServices' install entries (install.hpp), each the files of one old
// aggregator position, in their old order.
namespace alchemy::gui {

void install_coreservices_collation_and_types(export_fill::Provider& provider, ObjcRuntime& objc, CoreFoundation& cf) {
    export_fill::install_coreservices(provider, objc, cf);
}

void install_coreservices_uttypes(support_kit::Support& support) { completion_kit::data::install_uttypes(support); }

void install_coreservices_system_services(values::Provider& frameworks) { frameworks.install_services(); }

} // namespace alchemy::gui
