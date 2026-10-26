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

// vDSP kernels, BLAS/LAPACK and vImage conversions (vdsp.cpp; fill-only). It also installs:
void install_accelerate(InstallContext& context);
void install_vdsp_vectors(InstallContext& context);   // vdsp_vectors.cpp: the wider vDSP API, float and double
void install_vforce(InstallContext& context);         // vforce.cpp: vForce
void install_vimage(InstallContext& context);         // vimage.cpp: vImage buffers, conversions, geometry, convolution

} // namespace alchemy::gui::fill
