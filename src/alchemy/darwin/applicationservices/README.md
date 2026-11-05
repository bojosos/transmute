# ApplicationServices

`/System/Library/Frameworks/ApplicationServices.framework` (the umbrella).

* **Real:** the identity, and its re-exports: a symbol bound to the umbrella
  resolves through the CoreGraphics, CoreText, ImageIO, ColorSync and
  CoreServices providers (`install.cpp`, a `base_resolver` link). HIServices'
  Process Manager (GetCurrentProcess, SetFrontProcess, TransformProcessType,
  GetFrontProcess, SameProcess, GetProcessPID, CopyProcessName,
  ProcessInformationCopyDictionary) is a headless addition
  (`process_manager.cpp`, both builds). The guest is the only Mac process:
  GetFrontProcess names it while its application is active and otherwise
  answers procNotFound (a host program is in front); the name is the
  bundle's CFBundleName, else CFBundleExecutable, else the executable's file
  name, else the program name (empty when there is none of these); the information dictionary holds CFBundleName, CFBundleIdentifier,
  CFBundleExecutable (the path), BundlePath (for an .app), LSBackgroundOnly,
  LSUIElement, pid, PSN and IsHiddenAttr (bundle keys only when bundled), and
  the other processes get procNotFound / NULL. And
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
