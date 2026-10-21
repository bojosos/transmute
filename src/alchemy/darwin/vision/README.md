# Vision

`/System/Library/Frameworks/Vision.framework/Versions/A/Vision` (alchemy-fx).

* **Real:** The barcode symbology and error-domain constants; requests and the image request handler keep what they are given.
* **Unavailable, as documented:** No request can be performed: `-performRequests:error:` returns NO with an NSError (domain `com.apple.Vision`, code 8, VNErrorNotImplemented), and no barcode symbology is supported. A host vision runtime (Windows.Media.Ocr, a barcode decoder) would make parts of it real.
* **Host dependencies:** none.
* **Registration:** `install_vision`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/vision_tests.cpp` (ctest `alchemy_gui_vision`).
