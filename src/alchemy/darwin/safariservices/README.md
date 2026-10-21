# SafariServices

`/System/Library/Frameworks/SafariServices.framework/Versions/A/SafariServices` (alchemy-fx).

* **Real:** Nothing beyond the answer below.
* **Unavailable, as documented:** No application claims a universal link: `-[SFUniversalLink initWithWebpageURL:]` returns nil, as documented for an unclaimed link.
* **Host dependencies:** none.
* **Registration:** `install_safariservices`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/safariservices_tests.cpp` (ctest `alchemy_gui_safariservices`).
