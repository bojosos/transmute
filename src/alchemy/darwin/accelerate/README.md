# Accelerate

`/System/Library/Frameworks/Accelerate.framework` (alchemy-fx).

* **Real:** vDSP kernels (vadd, vsub, vmul, vsadd, vsmul, vsma, vclip,
  maxmgv, svesq, conv, deq22D, ctoz/ztoc, zvmul, maxv, rmsqv, vclr, vlint,
  vmaxmg, vmmsb, vramp), the radix-2 FFT setups and `fft_zrip`, the LP64
  BLAS/LAPACK entries `cblas_dgemm` and `dgesv_`, and vImage's 8-bit
  Y'CbCr <-> ARGB conversions (`vdsp.cpp`), computed in double precision on
  the host.
* **Real, the wider vDSP API (`vdsp_vectors.cpp`), float and double (`D`):**
  * vector arithmetic (vdiv, vmax/vmin, vmaxmg/vminmg, vma, vam, vmsa, vabs, vneg, vsq, vssq,
    vfill, vgen, vrvrs);
  * scalar forms (vsdiv, svdiv, vsmsa, vlim, vthr, vthres, vthrsc);
  * reductions (sve, svemg, meanv, meanmgv, measqv, minv, minmgv, dotpr, and the index forms
    maxvi, minvi, maxmgvi, minmgvi);
  * integer and precision conversions (vflt*, vfix*, vfixr*, vfixu*, vspdp, vdpsp) and vdbcon;
  * Hann, Hamming and Blackman windows; mmul and mtrans;
  * split-complex arithmetic (zvadd, zvsub, zvmags, zvabs, zvphas, zvconj, zvmov);
  * complex and real FFTs (fft_zip/zop/zrop) and the DFT interface (`DFT_zop`/`zrop` for lengths
    f·2ⁿ, f ∈ {1, 3, 5, 15});
  * cascaded biquads and desamp.

  vdsp.cpp's own kernels keep their names; this file adds the rest.
* **Real, vForce (`vforce.cpp`):** element-wise sqrt, rsqrt, rec, exponentials and logarithms,
  rounding, the trigonometric and hyperbolic functions and their inverses, pow/pows, atan2, div,
  fmod, remainder, copysign, nextafter, sincos and cosisin, in float and double.
* **Real, vImage (`vimage.cpp`), beyond vdsp.cpp's Y'CbCr conversions:**
  * buffers (`vImageBuffer_Init` through the guest's malloc, `vImageCopyBuffer`, fills, channel
    overwrites);
  * Planar8/PlanarF, ARGB8888/Planar8, RGB888, RGB565 and channel-permute conversions;
  * premultiplying and unpremultiplying (ARGB and RGBA);
  * reflections, 90-degree rotations and Lanczos scaling of ARGB8888, Planar8, PlanarF (and
    ARGBFFFF scaling);
  * box, tent and general convolutions with the four edge styles;
  * histograms;
  * more conversions (RGB888 with RGBA/BGRA and planes, ARGB1555, 16U, half floats, ARGBFFFF planes,
    channel extraction), flattening and premultiplied blending, the ARGB8888 matrix multiply, table
    look-ups, equalization, Max/Min, rotation by any angle and affine warps (bilinear), and scaling
    of the 16-bit, CbCr8 and half-float formats.

  Temporary buffers aren't used: a temp-size query answers 0.
* **Real, vImage and CoreGraphics (`vimage_cg.cpp`):** `vImageBuffer_InitWithCGImage`,
  `vImageCreateCGImageFromBuffer`, `vImageConverter_CreateWithCGImageFormat` with
  `vImageConvert_AnyToAny`, and the format and size helpers, for gray and RGB images with 8-bit,
  16-bit or float components, any alpha placement and the default, 16Little and 32Little byte orders.
  There is no colour management: RGB becomes gray by Rec. 709 luma.
* **Real, more vDSP (`vdsp_more.cpp`), float and double:** compound arithmetic (vasm, vsbm, vaam,
  vmma, vintb, vpythg, ...), applied ramps, clipping with counts, running sums and integrals,
  normalize, zero crossings, sorting and gathering, polynomials and table interpolation, polar and
  rectangular forms, deq22, conv (D), 2-D image filters, the remaining split-complex operations,
  2-D complex FFTs; DCT types II-IV (`vdsp_vectors.cpp`).
* **Real, BLAS (`blas.cpp`):** CBLAS levels 1-3 in s/d/c/z (row- or column-major), the Fortran
  subroutines (`?gemm_`, `?gemv_`, `?axpy_`, `?scal_`, `?copy_`, `?swap_`, `?trsm_`, `?syrk_`, `?ger_`)
  and their `$NEWLAPACK` names with ?dot, ?nrm2, ?asum, i?amax, and the legacy LAPACK names
  (`?gesv_`, `?getrf_`, `?gesdd_`, ...) as the `$NEWLAPACK` drivers. The original interface's REAL
  functions (`sdot_` and the like) are not offered.
* **Real, LAPACK decomposition drivers (`lapack.cpp`, both builds):** `?gesdd` (SVD by one-sided
  Jacobi), `?syevd`/`?heevd` (symmetric and Hermitian eigen by cyclic Jacobi), `?geev` (general eigen:
  Hessenberg reduction and shifted QR to complex Schur form, real pairs packed as LAPACK does),
  `?geqrf` and `?orgqr`/`?ungqr` (Householder QR in LAPACK's representation), for s/d/c/z under the
  `$NEWLAPACK` names numpy links. They're installed by `install_darwin_numeric`
  (`libsystem/misc/numeric.cpp`) next to its BLAS, LU, Cholesky and least-squares entries.
* **Unavailable, as documented:** BNNS: no filter can be created
  (`BNNSFilterCreateLayer*` return NULL), applying one fails and only NULL
  may be destroyed (`bnns.cpp`). FFT setups other than radix 2 return NULL.
* **Host dependencies:** none (pure computation).
* **Registration:** `install_accelerate_bnns`, a row in the position of the
  former `install_framework_gaps` (a renderer's denoiser library first imported
  BNNS); `gui::fill::install_accelerate` (vDSP; Chromium's audio code in the
  Qt WebEngine images, then QLab) is installed by the fill-only kit
  (`kit/fwgaps/install.cpp`) at its old position.
* **Tests:** `tests/accelerate_tests.cpp` (ctest `alchemy_gui_accelerate`).
