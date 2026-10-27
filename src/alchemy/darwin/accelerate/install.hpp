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
void install_blas(InstallContext& context);           // blas.cpp: BLAS levels 1-3 and the legacy LAPACK names
void install_vdsp_more(InstallContext& context);      // vdsp_more.cpp: compound, complex, sorting, filters, 2-D FFTs
void install_vimage_cg(InstallContext& context);      // vimage_cg.cpp: CGImage buffers and vImageConverter

} // namespace alchemy::gui::fill
