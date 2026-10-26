# CoreML

`/System/Library/Frameworks/CoreML.framework/Versions/A/CoreML` (alchemy-fx).

* **Real:** MLModelConfiguration and MLPredictionOptions are value objects; MLMultiArray keeps Double, Float32, Float16 or Int32 elements in guest memory (its own, or the caller's with any strides) and converts them through NSNumber subscripts; MLFeatureValue holds Int64, Double, String, multi-array, dictionary and image-buffer values.
* **Unavailable, as documented:** No model can be loaded or compiled: the class methods return nil with an NSError (domain `com.apple.CoreML`, code 0, MLModelErrorGeneric), and the asynchronous ones complete with it. MLComputePlan loads fail the same way, so no compute device (MLCPUComputeDevice and the others) is ever handed out. A host inference runtime (DirectML or ONNX Runtime) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_coreml`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/coreml_tests.cpp` (ctest `alchemy_gui_coreml`).
