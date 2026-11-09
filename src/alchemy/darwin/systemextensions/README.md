# SystemExtensions

`/System/Library/Frameworks/SystemExtensions.framework/Versions/A/SystemExtensions` (alchemy-fx).

* **Real:** `OSSystemExtensionRequest` objects (`+activationRequestForExtension:queue:`,
  `+deactivationRequestForExtension:queue:`, `+propertiesRequestForExtension:queue:`) keep their
  identifier, queue and delegate; `OSSystemExtensionManager.sharedManager` takes them.
* **Unavailable, as documented:** the host has no system extensions (driver, network, endpoint
  security or camera extensions). `-submitRequest:` activates nothing: the delegate's
  `request:didFailWithError:` runs on the request's queue (the main queue without one) with
  `OSSystemExtensionErrorDomain` code 10, `OSSystemExtensionErrorForbiddenBySystemPolicy`.
* **Host dependencies:** none.
* **Registration:** `install_systemextensions`, one row of the installer table in the fill-only set
  (through `gui::values::Provider`). OBS Studio's virtual camera first linked the framework.
* **Tests:** `tests/systemextensions_tests.cpp` (ctest `alchemy_gui_systemextensions`).
