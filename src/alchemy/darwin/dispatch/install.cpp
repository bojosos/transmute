#include "alchemy/darwin/dispatch/install.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

namespace alchemy::dispatch {

void install_dispatch_data(gui::InstallContext& context) {
    export_fill::Provider p(context.process);
    export_fill::install_dispatch_data(p, context.objc);
}

} // namespace alchemy::dispatch
