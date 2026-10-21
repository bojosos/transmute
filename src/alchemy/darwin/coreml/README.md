# CoreML

`/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML` (alchemy-fx).

* **Real:** MLModelConfiguration is a real value object; the other model classes exist.
* **Unavailable, as documented:** No model can be loaded or compiled: the class methods return nil with an NSError (domain `com.apple.CoreML`, code 0, MLModelErrorGeneric), and the asynchronous ones complete with it. A host inference runtime (DirectML or ONNX Runtime) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_coreml`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/coreml_tests.cpp` (ctest `alchemy_gui_coreml`).
