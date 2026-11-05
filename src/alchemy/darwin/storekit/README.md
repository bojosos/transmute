# StoreKit

`/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit` (alchemy-fx).

* **Real:** `SKPayment`, `SKMutablePayment` (with `+paymentWithProduct:`), `SKProduct`, `SKProductsResponse` and `SKPaymentTransaction` are value objects. The default `SKPaymentQueue` keeps its transaction observers and transactions. `SKErrorDomain` is the domain string.
* **Unavailable, as documented:** There is no App Store, so the documented failures arrive through the usual callbacks on the main queue:
  * `+[SKPaymentQueue canMakePayments]` is NO, and the storefront is nil.
  * `addPayment:` adds a transaction in `SKPaymentTransactionStateFailed` with `SKErrorPaymentNotAllowed` (3), reported to the observers (`paymentQueue:updatedTransactions:`); `finishTransaction:` removes it (`paymentQueue:removedTransactions:`).
  * Restoring completed transactions fails (`paymentQueue:restoreCompletedTransactionsFailedWithError:`, `SKErrorUnknown`).
  * Product and receipt-refresh requests fail (`request:didFailWithError:`, `SKErrorUnknown`).
* **Host dependencies:** none.
* **Registration:** `install_storekit`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Electron's update
  framework (VS Code) first linked it.
* **Tests:** `tests/storekit_tests.cpp` (ctest `alchemy_gui_storekit`).
