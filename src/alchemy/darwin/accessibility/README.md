# Accessibility

`/System/Library/Frameworks/Accessibility.framework/Versions/A/Accessibility` (alchemy-fx).

* **Real:** AXCustomContent (label, value, importance) is a real value object.
* **Unavailable, as documented:** Nothing else of the framework is provided; no assistive technology reads the content.
* **Host dependencies:** none.
* **Registration:** `install_accessibility`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/accessibility_tests.cpp` (ctest `alchemy_gui_accessibility`).
