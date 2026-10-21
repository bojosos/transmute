# CoreHaptics

`/System/Library/Frameworks/CoreHaptics.framework/Versions/A/CoreHaptics` (alchemy-fx).

* **Real:** The AHAP key strings; CHHapticEventParameter, CHHapticDynamicParameter and CHHapticEvent are real value objects, and CHHapticPattern initializes.
* **Unavailable, as documented:** CHHapticEngine is not provided, so no pattern is ever played (no haptic device exists on the host).
* **Host dependencies:** none.
* **Registration:** `install_corehaptics`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/corehaptics_tests.cpp` (ctest `alchemy_gui_corehaptics`).
