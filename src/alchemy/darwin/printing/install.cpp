#include "alchemy/darwin/printing/printing.hpp"

namespace alchemy::gui::fill {

void install_printing(InstallContext& context) {
    install_cups(context);
}

} // namespace alchemy::gui::fill
