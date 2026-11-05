#pragma once

// Printing for strict mode (src/alchemy/darwin/printing): libcups, which Qt
// PrintSupport and Chromium link (no scheduler runs under Alchemy); installed
// by install_printing (fill-only). PrintCore (PM*) is print_core.cpp's.

#include "alchemy/gui/fill_provider.hpp"

namespace alchemy::gui::fill {

void install_cups(InstallContext& context);

} // namespace alchemy::gui::fill
