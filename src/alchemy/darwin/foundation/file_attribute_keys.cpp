#include "alchemy/darwin/foundation/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/object_store.hpp"

// NSFileModificationDate, when missing. a renderer plug-in's images first imported it
// (moved from darwin/identities/framework_gaps.cpp).
namespace alchemy::gui {

using namespace alchemy::export_fill;

void install_foundation_file_attribute_keys(InstallContext& context) {
    auto& process = context.process;
    auto& cf = context.cf;
    if (!process.resolve(foundation_library, "_NSFileModificationDate")) {
        const auto value = cf.string("NSFileModificationDate");
        if (const auto record = process.objects().find(value)) record->immortal = true;
        const auto cell = process.allocate_guest(8);
        process.main_engine().write64(cell, value);
        process.install_export(std::string(foundation_library), "_NSFileModificationDate", {cell, ExportKind::data});
    }
}

} // namespace alchemy::gui
