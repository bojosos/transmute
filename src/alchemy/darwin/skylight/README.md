# SkyLight

`/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight` (alchemy-fx).

* **Real:** The identity only.
* **Unavailable, as documented:** SkyLight is the private window-server client library; it has no exports here, so weak imports bind to NULL and callers fall back to public API. The window server is darwin/windowserver.
* **Host dependencies:** none.
* **Registration:** `install_skylight`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/skylight_tests.cpp` (ctest `alchemy_gui_skylight`).
