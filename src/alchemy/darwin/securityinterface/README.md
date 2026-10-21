# SecurityInterface

`/System/Library/Frameworks/SecurityInterface.framework` (alchemy-fx).

* **Real:** `SFCertificatePanel` is a class shell: a real NSPanel subclass
  with no methods of its own, so guest subclasses link (`install.cpp`).
* **Unavailable, as documented:** the panel shows no certificates; any
  message it does not inherit fails as an unsupported message.
* **Host dependencies:** none.
* **Registration:** installed by the fill-only kit (`kit/fwgaps/install.cpp`)
  at its old position (the Qt WebEngine images first linked it).
* **Tests:** `tests/securityinterface_tests.cpp` (ctest
  `alchemy_gui_securityinterface`).
