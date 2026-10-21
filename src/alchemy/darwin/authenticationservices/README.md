# AuthenticationServices

`/System/Library/Frameworks/AuthenticationServices.framework/Versions/A/AuthenticationServices` (alchemy-fx).

* **Real:** The WebAuthn string constants; request, descriptor and provider classes are real value objects; the delegate hears the completion on the main queue, as after a real attempt.
* **Unavailable, as documented:** No authenticator (passkeys, security keys, single sign-on) exists: `-performRequests` completes with `ASAuthorizationErrorNotHandled` (1003), the browser credential manager reports access as denied, and the system never starts a web authentication session. A host WebAuthn API (Windows Hello) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_authenticationservices`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/authenticationservices_tests.cpp` (ctest `alchemy_gui_authenticationservices`).
