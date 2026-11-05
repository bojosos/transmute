# AuthenticationServices

`/System/Library/Frameworks/AuthenticationServices.framework/Versions/A/AuthenticationServices` (alchemy-fx).

* **Real:** The WebAuthn, scope and operation string constants; request, descriptor and provider classes are real value objects (the Apple ID, password and platform public-key providers make their requests); the delegate hears the completion on the main queue, as after a real attempt.
* **Unavailable, as documented:** No authenticator (passkeys, security keys, single sign-on) exists: `-performRequests` completes with `ASAuthorizationErrorNotHandled` (1003), the browser credential manager reports access as denied, no Apple ID is signed in (credential state NotFound), and no web authentication session can start (`-canStart` and `-start` are NO; the system never starts one in this browser). A host WebAuthn API (Windows Hello) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_authenticationservices`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/authenticationservices_tests.cpp` (ctest `alchemy_gui_authenticationservices`).
