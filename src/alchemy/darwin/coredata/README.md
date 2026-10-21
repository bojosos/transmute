# CoreData

`/System/Library/Frameworks/CoreData.framework/Versions/A/CoreData` (alchemy-fx).

* **Real:** The identity only.
* **Unavailable, as documented:** CoreData has no exports here. A framework that links it without using it loads; anything that imports a CoreData name does not bind.
* **Host dependencies:** none.
* **Registration:** `install_coredata`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Electron's Mantle
  framework (VS Code) first linked it.
* **Tests:** `tests/coredata_tests.cpp` (ctest `alchemy_gui_coredata`).
