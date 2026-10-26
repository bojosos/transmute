# SecurityInterface

`/System/Library/Frameworks/SecurityInterface.framework` (alchemy-fx).

* **Real:** `SFCertificatePanel`, `SFCertificateTrustPanel` and
  `SFChooseIdentityPanel` are NSPanel subclasses whose settings (policies,
  help anchor, button titles, informative text, domain, showsHelp) are real
  values; `-[SFChooseIdentityPanel identity]` is nil until a panel runs.
* **Unavailable, explicit:** no certificate UI exists here, so the shared
  panels and every way of running a panel (`runModal...`,
  `beginSheetForWindow:...`) stop with an "unsupported" naming the API.
* **Host dependencies:** none.
* **Registration:** installed by the fill-only kit (`kit/fwgaps/install.cpp`)
  at its old position (the Qt WebEngine images first linked it).
* **Tests:** `tests/securityinterface_tests.cpp` (ctest
  `alchemy_gui_securityinterface`).
