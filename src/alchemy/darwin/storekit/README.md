# StoreKit

`/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit` (alchemy-fx).

* **Real:** `SKPayment` and `SKMutablePayment` are value objects. The default `SKPaymentQueue` takes and removes transaction observers. `SKErrorDomain` is the domain string.
* **Unavailable, as documented:** There is no App Store:
  * `+[SKPaymentQueue canMakePayments]` is NO.
  * The queue's `transactions` is empty, so its observers are never called.
  * `addPayment:`, restoring completed transactions and `-[SKRequest start]` stop.
* **Host dependencies:** none.
* **Registration:** `install_storekit`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Electron's update
  framework (VS Code) first linked it.
* **Tests:** `tests/storekit_tests.cpp` (ctest `alchemy_gui_storekit`).
