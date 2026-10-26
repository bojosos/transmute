# ColorSync

`/System/Library/Frameworks/ColorSync.framework`.

* **Real (both builds, `install.cpp`):** the device-info keys
  (kColorSyncDeviceClass, ...).
* **Real (alchemy-fx, `profiles.cpp`):** ColorSyncProfileRef objects over ICC
  bytes, made from data, a file URL, or a name (the profiles CoreGraphics
  generates for its named color spaces: `coregraphics/icc_profiles.hpp`; the
  kColorSync*Profile names are those color spaces' names). The data, the
  128-byte header, tags by signature, the description ('desc' or 'mluc'), the
  MD5 (the header's profile ID, or the ICC.1 profile-ID digest when it is
  zero), and a structural verification. Data that is not an ICC profile gives
  NULL and an error.
* **Unavailable, as documented:** no display device is registered, so
  `CGDisplayCreateUUIDFromDisplayID`, `ColorSyncDeviceCopyDeviceInfo` and
  `ColorSyncProfileCreateWithDisplayID` return NULL. A host display-profile
  query could make them real.
* **Unsupported (explicit failure):** `ColorSyncTransformCreate` (no color
  management module converts between ICC profiles). Not registered: mutable
  profiles, installed-profile iteration, code fragments and device
  registration.
* **Host dependencies:** MD5 (`host::crypto`) for the profile ID.
* **Registration:** `darwin_headless::install_colorsync` (the headless
  framework set, every build) and `install_colorsync_profiles` (a row of the
  GUI installer table, alchemy-fx).
* **Tests:** `tests/colorsync_tests.cpp` (ctest `alchemy_gui_colorsync`) and
  the headless `alchemy_frameworks`.
