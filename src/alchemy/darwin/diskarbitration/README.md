# DiskArbitration

`/System/Library/Frameworks/DiskArbitration.framework` (alchemy-fx).

* **Real:** the identity (`install.cpp`); sessions, disks and their
  descriptions over the guest's volumes: a disk for `/` or an existing
  `/Volumes/<name>`, with its path, name, size, UUIDs and kind
  (`sessions.cpp`).
* **Unavailable, as documented:** no volume is ever ejected or unmounted,
  so approval callbacks are recorded but never called; disks cannot be
  claimed (`DADiskClaim` stops with "unsupported") and unclaiming does
  nothing (`claims.cpp`). Without a session, the description keys and "no
  daemon" answers come from `fill::install_devices` (darwin/iokit/).
* **Host dependencies:** none (the guest's file system).
* **Registration:** `install_diskarbitration` (the identity), a row in the
  position of the former `install_framework_gaps`; `sessions.cpp` is
  installed by the completion kit (`kit/completion/install.cpp`, QLab checks its
  license against the mounted volumes) and `claims.cpp` by the values kit
  (`darwin/coreservices/services.cpp` calls it in place). Qt Core and an
  installer link the identity without importing from it.
* **Tests:** `tests/diskarbitration_tests.cpp` (ctest
  `alchemy_gui_diskarbitration`).
