#pragma once

// Printing for strict mode (src/alchemy/printing): libcups and the PrintCore
// (PM*) functions of ApplicationServices that Qt PrintSupport and Chromium
// link. No printing system runs under Alchemy; installed by install_printing
// (fill-only).

#include "alchemy/gui/fill_provider.hpp"

namespace alchemy::gui::fill {

void install_cups(InstallContext& context);
void install_print_core(InstallContext& context);

} // namespace alchemy::gui::fill
