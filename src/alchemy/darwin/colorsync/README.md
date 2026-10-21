# ColorSync

`/System/Library/Frameworks/ColorSync.framework` (both builds).

* **Real:** the device-info keys (kColorSyncDeviceClass, ...).
* **Unavailable, as documented:** no display device is registered, so
  `CGDisplayCreateUUIDFromDisplayID` and `ColorSyncDeviceCopyDeviceInfo`
  return NULL. A host display-profile query could make it real.
* **Host dependencies:** none.
* **Registration:** `darwin_headless::install_colorsync`, in the headless
  framework set (`darwin/frameworks.cpp`, every build).
* **Tests:** `tests/colorsync_tests.cpp` (ctest `alchemy_gui_colorsync`) and
  the headless `alchemy_frameworks`.
