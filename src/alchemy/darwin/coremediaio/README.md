# CoreMediaIO

`/System/Library/Frameworks/CoreMediaIO.framework/Versions/A/CoreMediaIO` (alchemy-fx).

* **Real:** Nothing beyond the answers below.
* **Unavailable, as documented:** No DAL (camera) object exists: the property calls fail with `kCMIOHardwareBadObjectError` ('!obj') and `CMIOObjectHasProperty` is false. A host camera API (Media Foundation) would make it real.
  The DAL plug-in and camera extension side (OBS's virtual camera) runs only in the system's camera host:
  `CMIOObjectCreate` and `CMIOObjectsPublishedAndDied` answer '!obj', `CMIODeviceStart/StopStream` '!dev',
  `CMIOStreamCopyBufferQueue` '!str', `CMIOStreamClockCreate` and `CMIOSampleBufferCreateForImageBuffer`
  `kCMIOHardwareUnsupportedOperationError` ('unop'); the `CMIOExtension*` classes are shells.
* **Unverified values:** the `CMIOExtensionProperty*` strings are their names; the public documentation gives no values.
* **Host dependencies:** none.
* **Registration:** `install_coremediaio`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/coremediaio_tests.cpp` (ctest `alchemy_gui_coremediaio`).
