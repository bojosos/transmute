# CryptoTokenKit

`/System/Library/Frameworks/CryptoTokenKit.framework/Versions/A/CryptoTokenKit` (alchemy-fx).

* **Real:** TKTokenWatcher exists and keeps its handlers.
* **Unavailable, as documented:** No smart-card token is ever present: `-tokenIDs` is empty and the insertion handler is never called. The host smart-card service (WinSCard) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_cryptotokenkit`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/cryptotokenkit_tests.cpp` (ctest `alchemy_gui_cryptotokenkit`).
