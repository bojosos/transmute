# Accelerate

`/System/Library/Frameworks/Accelerate.framework` (alchemy-fx).

* **Real:** vDSP kernels (vadd, vsub, vmul, vsadd, vsmul, vsma, vclip,
  maxmgv, svesq, conv, deq22D, ctoz/ztoc, zvmul, maxv, rmsqv, vclr, vlint,
  vmaxmg, vmmsb, vramp), the radix-2 FFT setups and `fft_zrip`, the LP64
  BLAS/LAPACK entries `cblas_dgemm` and `dgesv_`, and vImage's 8-bit
  Y'CbCr <-> ARGB conversions (`vdsp.cpp`), computed in double precision on
  the host.
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
