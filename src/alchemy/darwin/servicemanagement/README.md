# ServiceManagement

`/System/Library/Frameworks/ServiceManagement.framework` (alchemy-fx).

* **Real:** nothing beyond the answer below.
* **Unavailable, as documented:** no launchd exists, so `SMJobBless`
  returns false with a CFError (`CFErrorDomainLaunchd`, 2,
  kSMErrorInternalFailure) (`job_bless.cpp`). `SMAppService` (the main app,
  login items, launch agents and daemons) reports every service as not
  registered; `-registerAndReturnError:` fails with kSMErrorServiceUnavailable
  (7) and unregistering with kSMErrorJobNotFound (6), in the same domain.
* **Host dependencies:** none.
* **Registration:** installed by the values kit
  (`darwin/coreservices/services.cpp` calls it in place; Chrome first
  imported it).
* **Tests:** `tests/servicemanagement_tests.cpp` (ctest
  `alchemy_gui_servicemanagement`).
