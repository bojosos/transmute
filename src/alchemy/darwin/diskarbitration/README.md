# DiskArbitration

`/System/Library/Frameworks/DiskArbitration.framework` (alchemy-fx).

* **Real:** the identity (`install.cpp`); sessions, disks and their
  descriptions over the guest's volumes: a disk for `/` or an existing
  `/Volumes/<name>`, with its path, name, a stable BSD name, size, UUIDs and
  kind (`sessions.cpp`). Sessions run their callbacks on their run loops
  (CFRunLoopPerformBlock) or dispatch queue; once scheduled, peek and
  disk-appeared callbacks receive the volumes their match dictionaries
  select, then the idle callbacks run. Disk options are kept; CFGetTypeID
  knows sessions, disks and dissenters.
* **Unavailable, as documented:** the volumes are host volumes, so mount
  requests answer kDAReturnBusy, unmount and rename kDAReturnNotPermitted and
  eject kDAReturnUnsupported, as a dissenter to the request's callback; no
  request proceeds, so approval callbacks are recorded but never called, and
  disappeared or description-changed callbacks never run. Disks cannot be
  claimed (`DADiskClaim` stops with "unsupported") and unclaiming does
  nothing (`claims.cpp`). No IOKit media object exists
  (`DADiskCreateFromIOMedia` is NULL).
* **Host dependencies:** none (the guest's file system).
* **Registration:** `install_diskarbitration` (the identity), a row in the
  position of the former `install_framework_gaps`; `sessions.cpp` is
  installed by the completion kit (`kit/completion/install.cpp`, QLab checks its
  license against the mounted volumes) and `claims.cpp` by the values kit
  (`darwin/coreservices/services.cpp` calls it in place). Qt Core and an
  installer link the identity without importing from it.
* **Tests:** `tests/diskarbitration_tests.cpp` (ctest
  `alchemy_gui_diskarbitration`).
