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
