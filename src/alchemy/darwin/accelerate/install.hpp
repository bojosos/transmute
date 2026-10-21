#pragma once

#include "alchemy/gui/install.hpp"

// Accelerate's install entries (alchemy-fx only):
//   its own installer-table row:  install_accelerate_bnns (bnns.cpp), in the
//                                 former install_framework_gaps position
//   fwgaps (fill::install_fwgaps): gui::fill::install_accelerate (vdsp.cpp)
namespace alchemy::accelerate {

void install_accelerate_bnns(gui::InstallContext& context);

} // namespace alchemy::accelerate

namespace alchemy::gui::fill {

// vDSP kernels, BLAS/LAPACK and vImage conversions (vdsp.cpp; fill-only).
void install_accelerate(InstallContext& context);

} // namespace alchemy::gui::fill
