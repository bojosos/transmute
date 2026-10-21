# ApplicationServices

`/System/Library/Frameworks/ApplicationServices.framework` (the umbrella).

* **Real:** the identity, and its re-exports: a symbol bound to the umbrella
  resolves through the CoreGraphics, CoreText, ImageIO, ColorSync and
  CoreServices providers (`install.cpp`, a `base_resolver` link). HIServices'
  Process Manager (GetCurrentProcess, SetFrontProcess, TransformProcessType)
  is a headless addition (`process_manager.cpp`, both builds), and
  the accessibility client (AXUIElement) in `darwin/coreservices/services.cpp`.
* **Unavailable, as documented:** Icon Services draws nothing:
  `PlotIconRefInContext` stops with an explicit "unsupported"
  (`icon_services.cpp`).
* **Host dependencies:** none.
* **Registration:** `install_applicationservices`, one installer-table row
  in the position of the former `install_framework_gaps` (a renderer plug-in's images
  first bound through the umbrella); `icon_services.cpp` is installed by the
  support kit (`kit/support/install.cpp`) at its old position.
* **Tests:** `tests/applicationservices_tests.cpp` (ctest
  `alchemy_gui_applicationservices`).
