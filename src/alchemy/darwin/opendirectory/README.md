# OpenDirectory

`/System/Library/Frameworks/OpenDirectory.framework` (alchemy-fx).

* **Real:** the attribute and record type constants (`constants.cpp`,
  `sessions.cpp`).
* **Unavailable, as documented:** no Open Directory daemon runs:
  `+[ODSession defaultSession]` is nil, and sessions and nodes cannot be
  opened (nil plus an NSError in `com.apple.OpenDirectory`, code 1002).
  ODQuery and ODRecord are classes with no methods of their own. A host
  directory service (the Windows account database) could make it real.
* **Host dependencies:** none.
* **Registration:** `sessions.cpp` is installed by the fill-only kit
  (`kit/fwgaps/install.cpp`; the Qt WebEngine images first imported it) and
  `constants.cpp` by the values kit (`darwin/coreservices/services.cpp` calls
  it in place; Chrome first imported those), each at its old position.
* **Tests:** `tests/opendirectory_tests.cpp` (ctest
  `alchemy_gui_opendirectory`).
