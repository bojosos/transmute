# ScreenCaptureKit

`/System/Library/Frameworks/ScreenCaptureKit.framework/Versions/A/ScreenCaptureKit` (alchemy-fx).

* **Real:** The content-sharing picker is a singleton that keeps its observers; filter, configuration and stream classes exist.
* **Unavailable, as documented:** Screen capture is not permitted: shareable content and screenshots complete with `SCStreamErrorUserDeclined` (-3801). Host screen capture (Windows.Graphics.Capture) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_screencapturekit`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/screencapturekit_tests.cpp` (ctest `alchemy_gui_screencapturekit`).
