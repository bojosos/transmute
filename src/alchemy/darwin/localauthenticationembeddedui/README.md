# LocalAuthenticationEmbeddedUI

`/System/Library/Frameworks/LocalAuthenticationEmbeddedUI.framework/Versions/A/LocalAuthenticationEmbeddedUI` (alchemy-fx).

* **Real:** LAAuthenticationView is a real NSView subclass that keeps its context.
* **Unavailable, as documented:** The view shows nothing: LocalAuthentication itself (darwin/iokit, `fill::install_devices`) has no biometry, so there is nothing to authenticate with.
* **Host dependencies:** none.
* **Registration:** `install_localauthenticationembeddedui`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/localauthenticationembeddedui_tests.cpp` (ctest `alchemy_gui_localauthenticationembeddedui`).
