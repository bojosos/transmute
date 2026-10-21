# QuickLookUI

`/System/Library/Frameworks/QuickLookUI.framework/Versions/A/QuickLookUI` (alchemy-fx).

* **Real:** `QLPreviewPanel` is a real `NSPanel` subclass.
* **Unavailable, as documented:** No Quick Look previews: `+sharedPreviewPanelExists` is NO, and asking for the shared panel (`+sharedPreviewPanel`) stops.
* **Host dependencies:** none.
* **Registration:** `install_quicklookui`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Electron's
  framework (VS Code) first linked it.
* **Tests:** `tests/quicklookui_tests.cpp` (ctest `alchemy_gui_quicklookui`).
