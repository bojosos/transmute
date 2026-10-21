#include "alchemy/darwin/compression/install.hpp"

#include "alchemy/darwin/kit/export_fill/libraries.hpp"

namespace alchemy::compression {

void install_zlib(gui::InstallContext& context) {
    export_fill::install_zlib(context.process);
    export_fill::install_zlib_stream(context.process);
}

} // namespace alchemy::compression
